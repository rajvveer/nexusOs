/* Hot-plug evdev + joydev devices (see evdev.h).
 *
 * evdev (/dev/input/eventN, char 13:64+N, N >= 2: 0 and 1 are the console keyboard and pointer of input.c): every fd
 * reads the device's event ring from where it opened (SYN_DROPPED after an overflow, like Linux), with
 * CLOCK_REALTIME or EVIOCSCLOCKID's clock. ioctls: EVIOCGVERSION/ID/NAME/PHYS/UNIQ/PROP/BIT/KEY/LED/SND/SW/ABS/
 * EFFECTS/MASK, EVIOCSABS, EVIOCSFF/RMFF (force feedback), EVIOCGRAB (while grabbed, the other evdev and joydev fds get
 * nothing; what happened meanwhile is never delivered to them),
 * EVIOCREVOKE, EVIOCSCLOCKID. write() of input_events: EV_FF play/stop and FF_GAIN, EV_LED (to the driver).
 * The input core's filtering applies: keys only on a state change (value 2 = repeat), absolute axes only on a change
 * beyond their fuzz, relative axes only when non-zero.
 * joydev (/dev/input/jsN, char 13:N) for devices that have joystick axes or buttons: struct js_event reads, starting
 * with the JS_EVENT_INIT state of every button and axis; buttons numbered BTN_JOYSTICK..KEY_MAX then
 * BTN_MISC..BTN_JOYSTICK-1, axes in ABS order, values scaled to -32767..32767 (joydev's default correction: the flat
 * centre is 0); JSIOCGVERSION/AXES/BUTTONS/NAME/AXMAP/BTNMAP/CORR, JSIOCSCORR. */
#include "evdev.h"
#include "hwnode.h"
#include "uaccess.h"
#include "../../kernel/string.h"
#include "../../kernel/vga.h"
/* Device state lives as long as the device, not the program that was running when it was plugged in: counted
 * allocations (storage_alloc.c), which the kernel's per-program allocation audit (kernel.c run_image) leaves out. */
#ifndef NEXUS_HOST_TEST
#include "storage_util.h"
#endif

#define DEVICES 16U
#define HANDLES 48U
#define QUEUE 1024U
#define JS_BUTTONS 512U
enum { E_PERM = -1, E_NOENT = -2, E_INTR = -4, E_AGAIN = -11, E_NOMEM = -12, E_FAULT = -14, E_BUSY = -16, E_NODEV = -19, E_INVAL = -22,
       E_NOSPC = -28, E_NOTTY = -25, E_NOSYS = -38 };
struct queued { uint64_t time; uint16_t type, code; int32_t value; };
struct effect { bool used; void* owner; struct evdev_ff_effect effect; bool playing; uint64_t until; };
struct evdev {
    bool used, gone; struct evdev_desc desc; const struct evdev_ops* ops; void* context;
    uint8_t key_state[96], led_state[2], snd_state[1], sw_state[2];
    struct queued events[QUEUE]; uint64_t sequence;
    int event_index, js_index, event_slot, js_slot, sys_input, sys_event, sys_js, input_number;
    void* grab;
    struct effect effects[EVDEV_FF_MAX]; unsigned gain; unsigned last_strong, last_weak;
    uint16_t js_buttons[JS_BUTTONS]; uint8_t js_axes[ABS_MAX + 1]; unsigned js_button_count, js_axis_count;
    int16_t js_button_of[KEY_MAX + 1];                                  /* key code -> joydev button number, -1 */
    int8_t js_axis_of[ABS_MAX + 1];
    struct js_corr { int32_t coef[8]; int16_t prec; uint16_t type; } js_corr[ABS_MAX + 1];   /* per joydev axis */
};
_Static_assert(sizeof(struct js_corr) == 36, "struct js_corr");
struct handle { bool used, js; struct evdev* device; uint64_t sequence, generation; int clock; bool dropped, init_pending; unsigned init_next; };
static struct evdev* devices;                                           /* DEVICES entries, allocated on first use (~400 KiB) */
static struct handle handles[HANDLES];
static uint64_t generations; static unsigned input_numbers;
static uint64_t realtime_offset(void) { return native_realtime_ns() - native_clock_ns(); }

bool evdev_is_joystick(const struct evdev_desc* d) {
    if (!evdev_test_bit(d->ev, EV_ABS) && !evdev_test_bit(d->ev, EV_KEY)) return false;
    for (unsigned k = BTN_JOYSTICK; k < BTN_DIGI; k++) if (evdev_test_bit(d->key, k)) return true;   /* joystick/gamepad buttons */
    if (evdev_test_bit(d->ev, EV_ABS) && evdev_test_bit(d->abs, ABS_X) && evdev_test_bit(d->abs, ABS_Y) &&
        !evdev_test_bit(d->key, BTN_MOUSE) && !evdev_test_bit(d->key, 0x14A /* BTN_TOUCH */) && !evdev_test_bit(d->key, BTN_DIGI)) return true;
    return false;
}
void* evdev_context(const struct evdev* device) { return device ? device->context : NULL; }
int evdev_index(const struct evdev* device) { return device ? device->event_index : -1; }
int evdev_js_index(const struct evdev* device) { return device ? device->js_index : -1; }
int evdev_input_number(const struct evdev* device) { return device ? device->input_number : -1; }

/* ---------- emitting ---------- */
static void push(struct evdev* d, unsigned type, unsigned code, int value) {
    d->events[d->sequence++ % QUEUE] = (struct queued){native_clock_ns(), (uint16_t)type, (uint16_t)code, value};
}
void evdev_emit(struct evdev* d, unsigned type, unsigned code, int value) {
    if (!d || d->gone || type >= 32 || !evdev_test_bit(d->desc.ev, type)) return;
    if (type == EV_KEY) {
        if (code > KEY_MAX || !evdev_test_bit(d->desc.key, code)) return;
        bool down = value != 0, old = evdev_test_bit(d->key_state, code);
        if (value != 2 && down == old) return;
        if (value == 2 && !old) return;
        if (down) evdev_set_bit(d->key_state, code); else d->key_state[code / 8] &= (uint8_t)~(1U << (code % 8));
    } else if (type == EV_ABS) {
        if (code > ABS_MAX || !evdev_test_bit(d->desc.abs, code)) return;
        struct evdev_absinfo* a = &d->desc.absinfo[code];
        int64_t delta = (int64_t)value - a->value; if (delta < 0) delta = -delta;
        if (value == a->value || (a->fuzz && delta < a->fuzz / 2)) return;
        a->value = value;
    } else if (type == EV_REL) { if (code >= 16 || !evdev_test_bit(d->desc.rel, code) || !value) return; }
    else if (type == EV_MSC) { if (code >= 8 || !evdev_test_bit(d->desc.msc, code)) return; }
    else if (type == EV_LED) { if (code >= 16 || !evdev_test_bit(d->desc.led, code)) return; if (value) evdev_set_bit(d->led_state, code); else d->led_state[code / 8] &= (uint8_t)~(1U << (code % 8)); }
    else if (type == EV_SW) { if (code >= 16 || !evdev_test_bit(d->desc.sw, code)) return; if (value) evdev_set_bit(d->sw_state, code); else d->sw_state[code / 8] &= (uint8_t)~(1U << (code % 8)); }
    else if (type == EV_SYN) return;
    push(d, type, code, value);
}
void evdev_sync(struct evdev* d) {
    if (!d || d->gone) return;
    if (d->sequence && d->events[(d->sequence - 1) % QUEUE].type == EV_SYN) return;   /* nothing since the last report */
    push(d, EV_SYN, SYN_REPORT, 0);
    task_wake_channel(d); event_source_changed(d, 1U);
}

/* ---------- force feedback ---------- */
static void ff_update(struct evdev* d) {
    if (!d->ops || !d->ops->ff_play) return;
    uint64_t strong = 0, weak = 0;
    for (unsigned i = 0; i < EVDEV_FF_MAX; i++) {
        const struct effect* e = &d->effects[i];
        if (!e->used || !e->playing) continue;
        if (e->effect.type == FF_RUMBLE) { strong += e->effect.u.rumble.strong_magnitude; weak += e->effect.u.rumble.weak_magnitude; }
        else if (e->effect.type == FF_PERIODIC || e->effect.type == FF_CONSTANT) {
            int level = e->effect.type == FF_PERIODIC ? e->effect.u.periodic.magnitude : e->effect.u.constant.level;
            unsigned magnitude = (unsigned)(level < 0 ? -level : level) * 2U; strong += magnitude; weak += magnitude;
        }
    }
    strong = strong * d->gain / 0xFFFF; weak = weak * d->gain / 0xFFFF;
    if (strong > 0xFFFF) strong = 0xFFFF;
    if (weak > 0xFFFF) weak = 0xFFFF;
    if (strong == d->last_strong && weak == d->last_weak) return;
    d->last_strong = (unsigned)strong; d->last_weak = (unsigned)weak;
    d->ops->ff_play(d, (unsigned)strong, (unsigned)weak);
}
static void ff_play(struct evdev* d, unsigned id, int count) {
    if (id >= EVDEV_FF_MAX || !d->effects[id].used) return;
    struct effect* e = &d->effects[id];
    e->playing = count > 0;
    uint64_t length = e->effect.replay_length ? (uint64_t)e->effect.replay_length * 1000000ULL : 0;
    e->until = e->playing && length ? native_clock_ns() + (uint64_t)e->effect.replay_delay * 1000000ULL + length * (uint64_t)count : 0;
    ff_update(d);
}
uint64_t evdev_next_event(uint64_t now) {                                /* when a playing effect ends; ~0 none */
    uint64_t next = ~0ULL; (void)now;
    for (unsigned i = 0; devices && i < DEVICES; i++) {
        const struct evdev* d = &devices[i]; if (!d->used || d->gone) continue;
        for (unsigned k = 0; k < EVDEV_FF_MAX; k++) if (d->effects[k].used && d->effects[k].playing && d->effects[k].until && d->effects[k].until < next) next = d->effects[k].until;
    }
    return next;
}
void evdev_poll(uint64_t now) {
    for (unsigned i = 0; i < DEVICES; i++) {
        if (!devices) return;
        struct evdev* d = &devices[i]; bool changed = false;
        if (!d->used || d->gone) continue;
        for (unsigned k = 0; k < EVDEV_FF_MAX; k++) if (d->effects[k].used && d->effects[k].playing && d->effects[k].until && now >= d->effects[k].until) { d->effects[k].playing = false; changed = true; }
        if (changed) ff_update(d);
    }
}
static void ff_release_owner(struct evdev* d, void* owner) {
    bool changed = false;
    for (unsigned k = 0; k < EVDEV_FF_MAX; k++) if (d->effects[k].used && d->effects[k].owner == owner) {
        if (d->ops && d->ops->ff_erase) d->ops->ff_erase(d, (int)k);
        changed |= d->effects[k].playing; d->effects[k].used = false; d->effects[k].playing = false;
    }
    if (changed) ff_update(d);
}

/* ---------- sysfs ---------- */
static const char* const INPUT_ATTRS[] = {"name", "phys", "uniq", "properties", "modalias", "uevent", "id/bustype", "id/vendor", "id/product", "id/version",
    "capabilities/ev", "capabilities/key", "capabilities/rel", "capabilities/abs", "capabilities/msc", "capabilities/led", "capabilities/snd",
    "capabilities/ff", "capabilities/sw"};
static const char* const NODE_ATTRS[] = {"dev", "uevent"};
static unsigned hex_append_case(char* out, unsigned at, unsigned capacity, uint64_t value, bool pad, const char* alphabet) {
    char digits[17]; unsigned n = 0;
    do { digits[n++] = alphabet[value & 15]; value >>= 4; } while (value || (pad && n < 16));
    while (n && at + 1 < capacity) out[at++] = digits[--n];
    out[at] = 0; return at;
}
static unsigned hex_append(char* out, unsigned at, unsigned capacity, uint64_t value, bool pad) { return hex_append_case(out, at, capacity, value, pad, "0123456789abcdef"); }
static unsigned decimal_append(char* out, unsigned at, unsigned capacity, unsigned value) {
    char digits[12]; unsigned n = 0; do { digits[n++] = (char)('0' + value % 10); value /= 10; } while (value);
    while (n && at + 1 < capacity) out[at++] = digits[--n];
    out[at] = 0; return at;
}
static unsigned text_append(char* out, unsigned at, unsigned capacity, const char* text) { while (*text && at + 1 < capacity) out[at++] = *text++; out[at] = 0; return at; }
/* Linux bitmap text: 64-bit words from the highest non-zero one down, separated by spaces ("0" when empty). */
static unsigned bitmap_append(char* out, unsigned at, unsigned capacity, const uint8_t* bits, unsigned bytes) {
    int words = (int)((bytes + 7) / 8), top = -1;
    for (int w = 0; w < words; w++) for (unsigned b = 0; b < 8 && (unsigned)w * 8 + b < bytes; b++) if (bits[w * 8 + b]) top = w;
    if (top < 0) return text_append(out, at, capacity, "0");
    for (int w = top; w >= 0; w--) {
        uint64_t value = 0; for (unsigned b = 0; b < 8 && (unsigned)w * 8 + b < bytes; b++) value |= (uint64_t)bits[w * 8 + b] << (8 * b);
        at = hex_append(out, at, capacity, value, w != top);
        if (w) at = text_append(out, at, capacity, " ");
    }
    return at;
}
static unsigned properties_append(const struct evdev* d, char* out, unsigned at, unsigned capacity) {
    at = text_append(out, at, capacity, "ID_INPUT=1\n");
    if (evdev_is_joystick(&d->desc)) at = text_append(out, at, capacity, "ID_INPUT_JOYSTICK=1\n");
    bool keyboard = evdev_test_bit(d->desc.key, 30 /* KEY_A */) && evdev_test_bit(d->desc.key, 28 /* KEY_ENTER */);
    if (keyboard) at = text_append(out, at, capacity, "ID_INPUT_KEY=1\nID_INPUT_KEYBOARD=1\n");
    if (evdev_test_bit(d->desc.rel, 0) && evdev_test_bit(d->desc.rel, 1) && evdev_test_bit(d->desc.key, BTN_MOUSE)) at = text_append(out, at, capacity, "ID_INPUT_MOUSE=1\n");
    if (evdev_test_bit(d->desc.abs, 0) && evdev_test_bit(d->desc.abs, 1) && evdev_test_bit(d->desc.key, BTN_MOUSE) && !evdev_is_joystick(&d->desc)) at = text_append(out, at, capacity, "ID_INPUT_TABLET=1\n");
    return at;
}
static int sysfs_read_input(void* context, const char* attribute, char* out, unsigned capacity) {
    const struct evdev* d = context; unsigned at = 0;
    if (!d->used) return -19;
    const struct evdev_desc* x = &d->desc;
    if (!strcmp(attribute, "name")) at = text_append(out, at, capacity, x->name);
    else if (!strcmp(attribute, "phys")) at = text_append(out, at, capacity, x->phys);
    else if (!strcmp(attribute, "uniq")) at = text_append(out, at, capacity, x->uniq);
    else if (!strcmp(attribute, "properties")) at = bitmap_append(out, at, capacity, x->prop, sizeof(x->prop));
    else if (!strncmp(attribute, "id/", 3)) {
        unsigned value = !strcmp(attribute + 3, "bustype") ? x->bustype : !strcmp(attribute + 3, "vendor") ? x->vendor : !strcmp(attribute + 3, "product") ? x->product : x->version;
        char digits[5]; for (unsigned i = 0; i < 4; i++) digits[i] = "0123456789abcdef"[(value >> (12 - 4 * i)) & 15]; digits[4] = 0;
        at = text_append(out, at, capacity, digits);
    } else if (!strncmp(attribute, "capabilities/", 13)) {
        const char* c = attribute + 13;
        const uint8_t* bits = !strcmp(c, "ev") ? x->ev : !strcmp(c, "key") ? x->key : !strcmp(c, "rel") ? x->rel : !strcmp(c, "abs") ? x->abs : !strcmp(c, "msc") ? x->msc :
                              !strcmp(c, "led") ? x->led : !strcmp(c, "snd") ? x->snd : !strcmp(c, "ff") ? x->ff : x->sw;
        unsigned bytes = !strcmp(c, "ev") ? sizeof(x->ev) : !strcmp(c, "key") ? sizeof(x->key) : !strcmp(c, "rel") ? sizeof(x->rel) : !strcmp(c, "abs") ? sizeof(x->abs) :
                         !strcmp(c, "msc") ? sizeof(x->msc) : !strcmp(c, "led") ? sizeof(x->led) : !strcmp(c, "snd") ? sizeof(x->snd) : !strcmp(c, "ff") ? sizeof(x->ff) : sizeof(x->sw);
        at = bitmap_append(out, at, capacity, bits, bytes);
    } else if (!strcmp(attribute, "modalias") || !strcmp(attribute, "uevent")) {
        if (!strcmp(attribute, "uevent")) {
            at = text_append(out, at, capacity, "PRODUCT="); at = hex_append(out, at, capacity, x->bustype, false); at = text_append(out, at, capacity, "/");
            at = hex_append(out, at, capacity, x->vendor, false); at = text_append(out, at, capacity, "/"); at = hex_append(out, at, capacity, x->product, false);
            at = text_append(out, at, capacity, "/"); at = hex_append(out, at, capacity, x->version, false);
            at = text_append(out, at, capacity, "\nNAME=\""); at = text_append(out, at, capacity, x->name); at = text_append(out, at, capacity, "\"\nPHYS=\"");
            at = text_append(out, at, capacity, x->phys); at = text_append(out, at, capacity, "\"\nUNIQ=\""); at = text_append(out, at, capacity, x->uniq);
            at = text_append(out, at, capacity, "\"\nEV="); at = bitmap_append(out, at, capacity, x->ev, sizeof(x->ev));
            at = text_append(out, at, capacity, "\nKEY="); at = bitmap_append(out, at, capacity, x->key, sizeof(x->key));
            at = text_append(out, at, capacity, "\nABS="); at = bitmap_append(out, at, capacity, x->abs, sizeof(x->abs));
            at = text_append(out, at, capacity, "\nFF="); at = bitmap_append(out, at, capacity, x->ff, sizeof(x->ff));
            at = text_append(out, at, capacity, "\n"); at = properties_append(d, out, at, capacity);
            at = text_append(out, at, capacity, "MODALIAS=");
        }
        at = text_append(out, at, capacity, "input:b"); char id[5];
        const uint16_t ids[4] = {x->bustype, x->vendor, x->product, x->version}; const char* letters = "bvpe";
        for (unsigned k = 0; k < 4; k++) { if (k) { char l[2] = {letters[k], 0}; at = text_append(out, at, capacity, l); }
            for (unsigned i = 0; i < 4; i++) id[i] = "0123456789ABCDEF"[(ids[k] >> (12 - 4 * i)) & 15]; id[4] = 0; at = text_append(out, at, capacity, id); }
        at = text_append(out, at, capacity, "-");
        const struct { char letter; const uint8_t* bits; unsigned first, last; } maps[] = {
            {'e', x->ev, 0, 0x1F}, {'k', x->key, 113 /* KEY_MIN_INTERESTING */, KEY_MAX}, {'r', x->rel, 0, 0x0F}, {'a', x->abs, 0, ABS_MAX},
            {'m', x->msc, 0, 0x07}, {'l', x->led, 0, 0x0F}, {'s', x->snd, 0, 0x07}, {'f', x->ff, 0, 0x7F}, {'w', x->sw, 0, 0x0F}};
        for (unsigned m = 0; m < sizeof(maps) / sizeof(maps[0]); m++) {
            char l[2] = {maps[m].letter, 0}; at = text_append(out, at, capacity, l);
            for (unsigned b = maps[m].first; b < maps[m].last; b++) if (evdev_test_bit(maps[m].bits, b)) { at = hex_append_case(out, at, capacity, b, false, "0123456789ABCDEF"); at = text_append(out, at, capacity, ","); }
        }
        at = text_append(out, at, capacity, "\n");
        return (int)at;
    } else return -2;
    return (int)text_append(out, at, capacity, "\n");
}
static int sysfs_read_node(void* context, const char* attribute, char* out, unsigned capacity, bool js) {
    const struct evdev* d = context; unsigned at = 0;
    if (!d->used) return -19;
    unsigned minor = js ? (unsigned)d->js_index : 64U + (unsigned)d->event_index;
    if (!strcmp(attribute, "dev")) { at = text_append(out, at, capacity, "13:"); at = decimal_append(out, at, capacity, minor); return (int)text_append(out, at, capacity, "\n"); }
    at = text_append(out, at, capacity, "MAJOR=13\nMINOR="); at = decimal_append(out, at, capacity, minor);
    at = text_append(out, at, capacity, js ? "\nDEVNAME=input/js" : "\nDEVNAME=input/event"); at = decimal_append(out, at, capacity, js ? (unsigned)d->js_index : (unsigned)d->event_index);
    at = text_append(out, at, capacity, "\n");
    return (int)properties_append(d, out, at, capacity);
}
static int sysfs_read_event(void* context, const char* attribute, char* out, unsigned capacity) { return sysfs_read_node(context, attribute, out, capacity, false); }
static int sysfs_read_js(void* context, const char* attribute, char* out, unsigned capacity) { return sysfs_read_node(context, attribute, out, capacity, true); }
static void uevent(struct evdev* d, const char* action, bool js) {
    char path[96] = "/devices/virtual/input/input", name[24] = "DEVNAME=input/", minor[16] = "MINOR=";
    unsigned at = (unsigned)strlen(path); at = decimal_append(path, at, sizeof(path), (unsigned)d->input_number);
    at = text_append(path, at, sizeof(path), js ? "/js" : "/event"); decimal_append(path, at, sizeof(path), js ? (unsigned)d->js_index : (unsigned)d->event_index);
    at = text_append(name, (unsigned)strlen(name), sizeof(name), js ? "js" : "event"); decimal_append(name, at, sizeof(name), js ? (unsigned)d->js_index : (unsigned)d->event_index);
    decimal_append(minor, (unsigned)strlen(minor), sizeof(minor), js ? (unsigned)d->js_index : 64U + (unsigned)d->event_index);
    const char* joystick = evdev_is_joystick(&d->desc) ? "ID_INPUT_JOYSTICK=1" : "ID_INPUT_KEY=1";
    const char* pairs[] = {"SUBSYSTEM=input", name, "MAJOR=13", minor, "ID_INPUT=1", joystick};
    storage_uevent(action, path, pairs, 6);
}

/* ---------- create / destroy ---------- */
static const struct hwnode_ops event_ops, js_ops;
static void joydev_maps(struct evdev* d) {
    d->js_button_count = d->js_axis_count = 0;
    for (unsigned k = 0; k <= KEY_MAX; k++) d->js_button_of[k] = -1;
    for (unsigned a = 0; a <= ABS_MAX; a++) d->js_axis_of[a] = -1;
    for (unsigned k = BTN_JOYSTICK; k <= KEY_MAX && d->js_button_count < JS_BUTTONS; k++) if (evdev_test_bit(d->desc.key, k)) { d->js_button_of[k] = (int16_t)d->js_button_count; d->js_buttons[d->js_button_count++] = (uint16_t)k; }
    for (unsigned k = BTN_MISC; k < BTN_JOYSTICK && d->js_button_count < JS_BUTTONS; k++) if (evdev_test_bit(d->desc.key, k)) { d->js_button_of[k] = (int16_t)d->js_button_count; d->js_buttons[d->js_button_count++] = (uint16_t)k; }
    for (unsigned a = 0; a <= ABS_MAX; a++) if (evdev_test_bit(d->desc.abs, a)) { d->js_axis_of[a] = (int8_t)d->js_axis_count; d->js_axes[d->js_axis_count++] = (uint8_t)a; }
    for (unsigned i = 0; i < d->js_axis_count; i++) {                   /* joydev's default broken-line correction */
        const struct evdev_absinfo* a = &d->desc.absinfo[d->js_axes[i]]; struct js_corr* c = &d->js_corr[i];
        memset(c, 0, sizeof(*c)); c->type = 1; c->prec = 0;
        int64_t center = ((int64_t)a->maximum + a->minimum) / 2, t = ((int64_t)a->maximum - a->minimum) / 2 - 2 * (int64_t)a->flat;
        c->coef[0] = (int32_t)(center - a->flat); c->coef[1] = (int32_t)(center + a->flat);
        if (t <= 0) continue;
        c->coef[2] = (int32_t)((1LL << 29) / t); c->coef[3] = c->coef[2];
    }
}
static int lowest_free(bool js) {
    for (int n = js ? 0 : 2; n < (js ? 32 : 96); n++) {
        bool taken = false;
        for (unsigned i = 0; i < DEVICES; i++) if (devices[i].used && (js ? devices[i].js_index : devices[i].event_index) == n) taken = true;
        if (!taken) return n;
    }
    return -1;
}
struct evdev* evdev_create(const struct evdev_desc* desc, const struct evdev_ops* ops, void* context) {
    struct evdev* d = NULL;
    if (!devices) devices = (struct evdev*)arena_alloc(DEVICES * sizeof(struct evdev));   /* zeroed */
    if (!devices) return NULL;
    for (unsigned i = 0; i < DEVICES && !d; i++) if (!devices[i].used) d = &devices[i];
    if (!d || !desc) return NULL;
    memset(d, 0, sizeof(*d)); d->desc = *desc; d->desc.name[EVDEV_NAME - 1] = d->desc.phys[63] = d->desc.uniq[63] = 0;
    evdev_set_bit(d->desc.ev, EV_SYN);
    if (d->desc.ff_max > EVDEV_FF_MAX) d->desc.ff_max = EVDEV_FF_MAX;
    if (!d->desc.ff_max) d->desc.ev[EV_FF / 8] &= (uint8_t)~(1U << (EV_FF % 8));
    for (unsigned a = 0; a <= ABS_MAX; a++) {                          /* sane ranges: max >= min, value inside */
        struct evdev_absinfo* x = &d->desc.absinfo[a];
        if (x->maximum < x->minimum) x->maximum = x->minimum;
        if (x->value < x->minimum) x->value = x->minimum;
        if (x->value > x->maximum) x->value = x->maximum;
    }
    d->ops = ops; d->context = context; d->gain = 0xFFFF; d->event_index = d->js_index = -1; d->used = true;
    d->event_index = lowest_free(false); d->js_index = evdev_is_joystick(&d->desc) ? lowest_free(true) : -1;
    d->event_slot = d->js_slot = d->sys_input = d->sys_event = d->sys_js = -1;
    if (d->event_index < 0) { d->used = false; return NULL; }
    joydev_maps(d);
    char path[32] = "/dev/input/event"; decimal_append(path, (unsigned)strlen(path), sizeof(path), (unsigned)d->event_index);
    d->event_slot = hwnode_add(path, 13, 64U + (unsigned)d->event_index, (unsigned)(d - devices), 0660, 1000, &event_ops, "input");
    if (d->js_index >= 0) {
        char js[32] = "/dev/input/js"; decimal_append(js, (unsigned)strlen(js), sizeof(js), (unsigned)d->js_index);
        d->js_slot = hwnode_add(js, 13, (unsigned)d->js_index, (unsigned)(d - devices), 0664, 1000, &js_ops, "input");
    }
    if (d->event_slot < 0 || (d->js_index >= 0 && d->js_slot < 0)) { hwnode_remove(d->event_slot); hwnode_remove(d->js_slot); d->used = false; return NULL; }
    d->input_number = (int)input_numbers++;
    char name[24] = "input"; decimal_append(name, 5, sizeof(name), (unsigned)d->input_number);
    d->sys_input = hwsys_add("input", name, -1, INPUT_ATTRS, sizeof(INPUT_ATTRS) / sizeof(INPUT_ATTRS[0]), sysfs_read_input, d);
    if (d->sys_input >= 0) {
        char event[24] = "event"; decimal_append(event, 5, sizeof(event), (unsigned)d->event_index);
        d->sys_event = hwsys_add("input", event, d->sys_input, NODE_ATTRS, 2, sysfs_read_event, d);
        if (d->js_index >= 0) { char js[24] = "js"; decimal_append(js, 2, sizeof(js), (unsigned)d->js_index); d->sys_js = hwsys_add("input", js, d->sys_input, NODE_ATTRS, 2, sysfs_read_js, d); }
    }
    char text[160] = "[64] INPUT "; unsigned at = text_append(text, (unsigned)strlen(text), sizeof(text), path);
    if (d->js_index >= 0) { at = text_append(text, at, sizeof(text), " + /dev/input/js"); at = decimal_append(text, at, sizeof(text), (unsigned)d->js_index); }
    at = text_append(text, at, sizeof(text), ": "); at = text_append(text, at, sizeof(text), d->desc.name); text_append(text, at, sizeof(text), "\n");
    vga_print(text);
    uevent(d, "add", false); if (d->js_index >= 0) uevent(d, "add", true);
    return d;
}
static bool open_handles(const struct evdev* d) { for (unsigned i = 0; i < HANDLES; i++) if (handles[i].used && handles[i].device == d) return true; return false; }
/* The table goes back to the page pool when the last device is gone (memory returns to its boot baseline). */
static void release_slot(struct evdev* d) {
    d->used = false;
    for (unsigned i = 0; i < DEVICES; i++) if (devices[i].used) return;
    arena_free((uint64_t)devices, DEVICES * sizeof(struct evdev)); devices = NULL;
}
void evdev_destroy(struct evdev* d) {
    if (!d || !d->used || d->gone) return;
    if (d->ops && d->ops->ff_play && (d->last_strong || d->last_weak)) d->ops->ff_play(d, 0, 0);
    d->gone = true; d->ops = NULL; d->context = NULL;
    hwnode_remove(d->event_slot); hwnode_remove(d->js_slot); hwsys_remove(d->sys_input);
    uevent(d, "remove", false); if (d->js_index >= 0) uevent(d, "remove", true);
    vga_print("[64] INPUT device removed: "); vga_print(d->desc.name); vga_print("\n");
    task_wake_channel(d); event_source_changed(d, 1U | 8U | 16U);
    if (!open_handles(d)) release_slot(d);                              /* else the last close frees the slot */
}

/* ---------- fds ---------- */
static struct handle* handle_of(const struct input_reader* reader) {
    unsigned i = reader ? (unsigned)(reader->sequence & 0xFF) : 0;
    if (!i || i > HANDLES || !handles[i - 1].used || handles[i - 1].generation != reader->sequence >> 8) return NULL;
    return &handles[i - 1];
}
/* End of a grab: the other fds skip what the grabber got (Linux never queued it for them). */
static void grab_release(struct evdev* d) {
    void* grabber = d->grab; d->grab = NULL;
    for (unsigned i = 0; i < HANDLES; i++) if (handles[i].used && handles[i].device == d && (void*)&handles[i] != grabber) handles[i].sequence = d->sequence;
}
static struct evdev* device_of(unsigned cookie) { return devices && cookie < DEVICES && devices[cookie].used && !devices[cookie].gone ? &devices[cookie] : NULL; }
static int open_check(unsigned cookie, uint64_t flags) {
    (void)flags; if (!device_of(cookie)) return E_NODEV;
    for (unsigned i = 0; i < HANDLES; i++) if (!handles[i].used) return 0;
    return -24;                                                          /* EMFILE-like: no handle left */
}
static void open_common(unsigned cookie, struct input_reader* reader, bool js) {
    struct evdev* d = device_of(cookie); if (!d) return;
    for (unsigned i = 0; i < HANDLES; i++) if (!handles[i].used) {
        handles[i] = (struct handle){.used = true, .js = js, .device = d, .sequence = d->sequence, .generation = ++generations, .init_pending = js};
        reader->sequence = handles[i].generation << 8 | (i + 1); return;
    }
}
static void event_open(unsigned cookie, uint64_t flags, struct input_reader* reader) { (void)flags; open_common(cookie, reader, false); }
static void js_open(unsigned cookie, uint64_t flags, struct input_reader* reader) { (void)flags; open_common(cookie, reader, true); }
static void close_common(unsigned cookie, uint64_t flags, struct input_reader* reader) {
    (void)cookie; (void)flags; struct handle* h = handle_of(reader); if (!h) return;
    struct evdev* d = h->device;
    if (!d->gone) ff_release_owner(d, h);
    h->used = false; reader->sequence = 0;
    if (d->grab == h) grab_release(d);
    if (d->gone && !open_handles(d)) release_slot(d);
}
static uint32_t ready_common(unsigned cookie, const struct input_reader* reader) {
    (void)cookie; const struct handle* h = handle_of(reader);
    if (!h || h->device->gone) return 8U | 16U;
    const struct evdev* d = h->device;
    if (h->js && h->init_pending) return 1U | 64U;
    if (d->grab && d->grab != h) return 0;
    return h->sequence != d->sequence ? 1U | 64U : 0;
}
static void* source_common(unsigned cookie, const struct input_reader* reader) { (void)cookie; const struct handle* h = handle_of(reader); return h ? (void*)h->device : NULL; }
struct input_event64 { int64_t seconds, microseconds; uint16_t type, code; int32_t value; };
static int64_t event_io(unsigned cookie, struct input_reader* reader, uint64_t buffer, uint64_t count, bool write, uint64_t flags) {
    (void)cookie; struct handle* h = handle_of(reader); if (!h) return E_NODEV;
    struct evdev* d = h->device; if (d->gone) return E_NODEV;
    if (count < sizeof(struct input_event64)) return E_INVAL;
    if (write) {                                                         /* EV_FF play/gain, EV_LED */
        uint64_t n = count / sizeof(struct input_event64), done = 0;
        if (!accessible(buffer, n * sizeof(struct input_event64), false)) return E_FAULT;
        for (; done < n; done++) {
            struct input_event64 e; copy_from_user(&e, (const void*)(buffer + done * sizeof(e)), sizeof(e));
            if (d->gone) break;
            if (e.type == EV_FF && evdev_test_bit(d->desc.ev, EV_FF)) {
                if (e.code == FF_GAIN) { if (evdev_test_bit(d->desc.ff, FF_GAIN)) { d->gain = e.value < 0 ? 0 : e.value > 0xFFFF ? 0xFFFF : (unsigned)e.value; ff_update(d); } }
                else if (e.code < EVDEV_FF_MAX) ff_play(d, e.code, e.value);
            } else if (e.type == EV_LED && e.code < 16 && evdev_test_bit(d->desc.led, e.code)) evdev_emit(d, EV_LED, e.code, e.value);
            if (d->ops && d->ops->client_event && (e.type == EV_FF || e.type == EV_LED || e.type == EV_SND)) d->ops->client_event(d, e.type, e.code, e.value);
        }
        return (int64_t)(done * sizeof(struct input_event64));
    }
    for (;;) {
        bool ready = (!d->grab || d->grab == h) && h->sequence != d->sequence;
        if (ready) break;
        if (flags & 0x800) return E_AGAIN;
        int error = task_wait_channel(d); if (error) return error;
        if (!h->used || d->gone) return E_NODEV;
    }
    uint64_t wanted = count / sizeof(struct input_event64), used = 0, offset = h->clock == 1 ? 0 : realtime_offset();
    if (!accessible(buffer, wanted * sizeof(struct input_event64), true)) return E_FAULT;
    while (used < wanted && h->sequence != d->sequence) {
        struct queued q;
        if (d->sequence - h->sequence > QUEUE) { q = (struct queued){native_clock_ns(), EV_SYN, SYN_DROPPED, 0}; h->sequence = d->sequence; }   /* overflow */
        else q = d->events[h->sequence++ % QUEUE];
        uint64_t t = q.time + offset;
        struct input_event64 out = {(int64_t)(t / 1000000000ULL), (int64_t)(t % 1000000000ULL / 1000), q.type, q.code, q.value};
        copy_to_user((void*)(buffer + used * sizeof(out)), &out, sizeof(out)); used++;
    }
    return (int64_t)(used * sizeof(struct input_event64));
}
static int put_bits(uint64_t argument, unsigned size, const void* data, unsigned length) {
    unsigned n = size < length ? size : length;
    if (n && !accessible(argument, n, true)) return E_FAULT;
    if (n) copy_to_user((void*)argument, data, n);
    return (int)n;
}
static int64_t event_ioctl(unsigned cookie, struct input_reader* reader, uint64_t command, uint64_t argument, uint64_t flags) {
    (void)cookie; (void)flags; struct handle* h = handle_of(reader); if (!h) return E_NODEV;
    struct evdev* d = h->device; if (d->gone) return E_NODEV;
    unsigned nr = command & 0xFF, type = (command >> 8) & 0xFF, size = (command >> 16) & 0x3FFF, dir = (unsigned)(command >> 30) & 3;
    if (type != 'E') return E_NOTTY;
    const struct evdev_desc* x = &d->desc;
    if (dir == 2) switch (nr) {
        case 0x01: { int32_t v = 0x010001; return put_bits(argument, size, &v, 4) < 0 ? E_FAULT : 0; }
        case 0x02: { uint16_t id[4] = {x->bustype, x->vendor, x->product, x->version}; return put_bits(argument, size, id, 8) < 0 ? E_FAULT : 0; }
        case 0x03: { uint32_t rep[2] = {250, 33}; return evdev_test_bit(x->ev, EV_REP) ? (put_bits(argument, size, rep, 8) < 0 ? E_FAULT : 0) : E_NOSYS; }
        case 0x06: return put_bits(argument, size, x->name, (unsigned)strlen(x->name) + 1);
        case 0x07: return x->phys[0] ? put_bits(argument, size, x->phys, (unsigned)strlen(x->phys) + 1) : E_NOENT;
        case 0x08: return x->uniq[0] ? put_bits(argument, size, x->uniq, (unsigned)strlen(x->uniq) + 1) : E_NOENT;
        case 0x09: return put_bits(argument, size, x->prop, sizeof(x->prop));
        case 0x18: return put_bits(argument, size, d->key_state, sizeof(d->key_state));
        case 0x19: return put_bits(argument, size, d->led_state, sizeof(d->led_state));
        case 0x1A: return put_bits(argument, size, d->snd_state, sizeof(d->snd_state));
        case 0x1B: return put_bits(argument, size, d->sw_state, sizeof(d->sw_state));
        case 0x84: { int32_t v = (int32_t)x->ff_max; return put_bits(argument, size, &v, 4) < 0 ? E_FAULT : 0; }
    }
    if (dir == 2 && nr >= 0x20 && nr < 0x40) {                          /* EVIOCGBIT(type, len) */
        unsigned t = nr - 0x20; uint8_t zero[96] = {0};
        const uint8_t* bits = t == 0 ? x->ev : t == EV_KEY ? x->key : t == EV_REL ? x->rel : t == EV_ABS ? x->abs : t == EV_MSC ? x->msc :
                              t == EV_LED ? x->led : t == EV_SND ? x->snd : t == EV_FF ? x->ff : t == EV_SW ? x->sw : zero;
        unsigned bytes = t == 0 ? sizeof(x->ev) : t == EV_KEY ? sizeof(x->key) : t == EV_REL ? sizeof(x->rel) : t == EV_ABS ? sizeof(x->abs) : t == EV_MSC ? sizeof(x->msc) :
                         t == EV_LED ? sizeof(x->led) : t == EV_SND ? sizeof(x->snd) : t == EV_FF ? sizeof(x->ff) : t == EV_SW ? sizeof(x->sw) : 0;
        if (!bytes && t != EV_REP && t != EV_PWR && t != EV_FF_STATUS) return E_INVAL;
        if (!size) return 0;
        uint8_t out[96] = {0}; unsigned n = size < sizeof(out) ? size : sizeof(out); if (bytes) memcpy(out, bits, bytes < n ? bytes : n);
        return put_bits(argument, size, out, n);
    }
    if (dir == 2 && nr >= 0x40 && nr < 0x80 && size >= 24) {            /* EVIOCGABS(abs) */
        unsigned a = nr - 0x40; if (!evdev_test_bit(x->abs, a)) return E_INVAL;
        return put_bits(argument, 24, &x->absinfo[a], 24) < 0 ? E_FAULT : 0;
    }
    if (dir == 1 && nr >= 0xC0 && size >= 24) {                         /* EVIOCSABS(abs) */
        unsigned a = nr - 0xC0; if (a > ABS_MAX || !evdev_test_bit(x->abs, a)) return E_INVAL;
        struct evdev_absinfo info; if (!accessible(argument, 24, false)) return E_FAULT; copy_from_user(&info, (const void*)argument, 24);
        if (info.maximum < info.minimum) return E_INVAL;
        d->desc.absinfo[a] = info; return 0;
    }
    if (dir == 1 && nr == 0x80 && size == sizeof(struct evdev_ff_effect)) {   /* EVIOCSFF */
        if (!evdev_test_bit(x->ev, EV_FF) || !x->ff_max) return E_INVAL;
        struct evdev_ff_effect e; if (!accessible(argument, sizeof(e), true)) return E_FAULT; copy_from_user(&e, (const void*)argument, sizeof(e));
        if (e.type < 0x50 || e.type > 0x57 || !evdev_test_bit(x->ff, e.type)) return E_INVAL;
        if (e.type == FF_PERIODIC && (e.u.periodic.waveform < 0x58 || e.u.periodic.waveform > 0x5D || !evdev_test_bit(x->ff, e.u.periodic.waveform))) return E_INVAL;
        int id = e.id;
        if (id == -1) { id = -1; for (unsigned k = 0; k < x->ff_max; k++) if (!d->effects[k].used) { id = (int)k; break; } if (id < 0) return E_NOSPC; }
        else if (id < 0 || (unsigned)id >= x->ff_max || !d->effects[id].used || d->effects[id].owner != h) return E_INVAL;
        e.id = (int16_t)id;
        struct evdev_ff_effect old = d->effects[id].effect; bool fresh = !d->effects[id].used;
        if (fresh) d->effects[id] = (struct effect){.used = true, .owner = h, .effect = e};   /* reserved while the owner answers */
        if (d->ops && d->ops->ff_upload) {
            int error = d->ops->ff_upload(d, &e, fresh ? NULL : &old);
            if (!error && d->gone) error = E_NODEV;
            if (error) { if (fresh && d->effects[id].owner == h) d->effects[id].used = false; return error; }
        }
        d->effects[id].used = true; d->effects[id].owner = h; d->effects[id].effect = e;
        if (d->effects[id].playing) ff_update(d);
        copy_to_user((void*)argument, &e, sizeof(e));                     /* the id comes back */
        return 0;
    }
    if (dir == 1 && nr == 0x81) {                                         /* EVIOCRMFF(id) */
        int id = (int)argument; if (id < 0 || (unsigned)id >= EVDEV_FF_MAX || !d->effects[id].used || d->effects[id].owner != h) return E_INVAL;
        if (d->ops && d->ops->ff_erase) { int error = d->ops->ff_erase(d, id); if (error) return error; }
        bool was = d->effects[id].playing; d->effects[id].used = d->effects[id].playing = false; if (was) ff_update(d);
        return 0;
    }
    if (dir == 1 && nr == 0x90) {                                         /* EVIOCGRAB */
        if (argument) { if (d->grab) return E_BUSY; d->grab = h; } else { if (d->grab != h) return E_INVAL; grab_release(d); }
        return 0;
    }
    if (dir == 1 && nr == 0x91) { if (argument) return E_INVAL; ff_release_owner(d, h); h->used = false; reader->sequence = 0; if (d->grab == h) grab_release(d); task_wake_channel(d); return 0; }   /* EVIOCREVOKE */
    if (dir == 1 && nr == 0xA0) {                                         /* EVIOCSCLOCKID */
        int32_t clock; if (!accessible(argument, 4, false)) return E_FAULT; copy_from_user(&clock, (const void*)argument, 4);
        if (clock != 0 && clock != 1 && clock != 7) return E_INVAL;
        h->clock = clock == 0 ? 0 : 1; return 0;
    }
    if (nr == 0x92 || nr == 0x93) return 0;                               /* EVIOCGMASK/SMASK: all events pass */
    return E_NOTTY;
}
/* ---------- joydev ---------- */
struct js_event { uint32_t time; int16_t value; uint8_t type, number; };
/* joydev_correct: JS_CORR_NONE passes the value, JS_CORR_BROKEN is the dead-zone line; clamped to +-32767. */
static int16_t js_axis_value(const struct evdev* d, unsigned code, int value) {
    int axis = d->js_axis_of[code]; if (axis < 0) return 0;
    const struct js_corr* c = &d->js_corr[axis]; int64_t v = value;
    if (c->type == 1) v = v > c->coef[0] ? (v < c->coef[1] ? 0 : ((int64_t)c->coef[3] * (v - c->coef[1])) >> 14) : ((int64_t)c->coef[2] * (v - c->coef[0])) >> 14;
    else if (c->type) return 0;
    return (int16_t)(v > 32767 ? 32767 : v < -32767 ? -32767 : v);
}
static int64_t js_io(unsigned cookie, struct input_reader* reader, uint64_t buffer, uint64_t count, bool write, uint64_t flags) {
    (void)cookie; struct handle* h = handle_of(reader); if (!h) return E_NODEV;
    struct evdev* d = h->device; if (d->gone) return E_NODEV;
    if (write) return E_INVAL;
    if (count < sizeof(struct js_event)) return E_INVAL;
    for (;;) {
        bool pending = h->init_pending;
        if (!pending && !d->grab) for (uint64_t s = h->sequence; s != d->sequence && !pending; s++) {
            if (d->sequence - s > QUEUE) { pending = true; break; }
            const struct queued* q = &d->events[s % QUEUE];
            pending = (q->type == EV_KEY && q->code <= KEY_MAX && d->js_button_of[q->code] >= 0) || (q->type == EV_ABS && q->code <= ABS_MAX && d->js_axis_of[q->code] >= 0);
            if (!pending) h->sequence = s + 1;
        }
        if (pending) break;
        if (flags & 0x800) return E_AGAIN;
        int error = task_wait_channel(d); if (error) return error;
        if (!h->used || d->gone) return E_NODEV;
    }
    uint64_t wanted = count / sizeof(struct js_event), used = 0;
    if (!accessible(buffer, wanted * sizeof(struct js_event), true)) return E_FAULT;
    uint32_t now_ms = (uint32_t)(native_clock_ns() / 1000000ULL);
    while (used < wanted) {
        struct js_event e;
        if (h->init_pending) {                                           /* the state of every button, then every axis */
            unsigned i = h->init_next++;
            if (i < d->js_button_count) e = (struct js_event){now_ms, (int16_t)evdev_test_bit(d->key_state, d->js_buttons[i]), 0x80 | 0x01, (uint8_t)i};
            else if (i < d->js_button_count + d->js_axis_count) { unsigned a = d->js_axes[i - d->js_button_count]; e = (struct js_event){now_ms, js_axis_value(d, a, d->desc.absinfo[a].value), 0x80 | 0x02, (uint8_t)(i - d->js_button_count)}; }
            else { h->init_pending = false; h->sequence = d->sequence; continue; }
        } else {
            if (h->sequence == d->sequence || d->grab) break;
            if (d->sequence - h->sequence > QUEUE) { h->init_pending = true; h->init_next = 0; continue; }   /* overflow: resend the state */
            const struct queued* q = &d->events[h->sequence++ % QUEUE];
            if (q->type == EV_KEY && q->code <= KEY_MAX && d->js_button_of[q->code] >= 0 && q->value != 2) e = (struct js_event){(uint32_t)(q->time / 1000000ULL), (int16_t)(q->value != 0), 0x01, (uint8_t)d->js_button_of[q->code]};
            else if (q->type == EV_ABS && q->code <= ABS_MAX && d->js_axis_of[q->code] >= 0) e = (struct js_event){(uint32_t)(q->time / 1000000ULL), js_axis_value(d, q->code, q->value), 0x02, (uint8_t)d->js_axis_of[q->code]};
            else continue;
        }
        copy_to_user((void*)(buffer + used * sizeof(e)), &e, sizeof(e)); used++;
    }
    return (int64_t)(used * sizeof(struct js_event));
}
static uint32_t js_ready(unsigned cookie, const struct input_reader* reader) {
    (void)cookie; const struct handle* h = handle_of(reader);
    if (!h || h->device->gone) return 8U | 16U;
    if (h->init_pending) return 1U | 64U;
    const struct evdev* d = h->device;
    if (d->grab) return 0;
    for (uint64_t s = h->sequence; s != d->sequence; s++) {
        if (d->sequence - s > QUEUE) return 1U | 64U;
        const struct queued* q = &d->events[s % QUEUE];
        if ((q->type == EV_KEY && q->code <= KEY_MAX && d->js_button_of[q->code] >= 0) || (q->type == EV_ABS && q->code <= ABS_MAX && d->js_axis_of[q->code] >= 0)) return 1U | 64U;
    }
    return 0;
}
static int64_t js_ioctl(unsigned cookie, struct input_reader* reader, uint64_t command, uint64_t argument, uint64_t flags) {
    (void)cookie; (void)flags; struct handle* h = handle_of(reader); if (!h) return E_NODEV;
    struct evdev* d = h->device; if (d->gone) return E_NODEV;
    unsigned nr = command & 0xFF, type = (command >> 8) & 0xFF, size = (command >> 16) & 0x3FFF, dir = (unsigned)(command >> 30) & 3;
    if (type != 'j') return E_NOTTY;
    if (dir == 2 && nr == 0x01) { uint32_t v = 0x020100; return put_bits(argument, 4, &v, 4) < 0 ? E_FAULT : 0; }
    if (dir == 2 && nr == 0x11) { uint8_t v = (uint8_t)d->js_axis_count; return put_bits(argument, 1, &v, 1) < 0 ? E_FAULT : 0; }
    if (dir == 2 && nr == 0x12) { uint8_t v = (uint8_t)(d->js_button_count > 255 ? 255 : d->js_button_count); return put_bits(argument, 1, &v, 1) < 0 ? E_FAULT : 0; }
    if (dir == 2 && nr == 0x13) return put_bits(argument, size, d->desc.name, (unsigned)strlen(d->desc.name) + 1);
    if (dir == 2 && nr == 0x32) { uint8_t map[ABS_MAX + 1] = {0}; memcpy(map, d->js_axes, d->js_axis_count); return put_bits(argument, size, map, sizeof(map)) < 0 ? E_FAULT : 0; }
    if (dir == 2 && nr == 0x34) { static uint16_t map[KEY_MAX - BTN_MISC + 1]; memset(map, 0, sizeof(map)); memcpy(map, d->js_buttons, d->js_button_count * 2); return put_bits(argument, size, map, sizeof(map)) < 0 ? E_FAULT : 0; }
    if (dir == 2 && nr == 0x22) {                                         /* JSIOCGCORR: one js_corr per axis (the size field says one) */
        unsigned bytes = d->js_axis_count * (unsigned)sizeof(struct js_corr);
        return put_bits(argument, bytes, d->js_corr, bytes) < 0 ? E_FAULT : 0;
    }
    if (dir == 1 && nr == 0x21) {                                         /* JSIOCSCORR: one js_corr per axis */
        unsigned bytes = d->js_axis_count * (unsigned)sizeof(struct js_corr);
        if (!bytes) return 0;
        if (!accessible(argument, bytes, false)) return E_FAULT;
        static struct js_corr corr[ABS_MAX + 1]; copy_from_user(corr, (const void*)argument, bytes);
        for (unsigned i = 0; i < d->js_axis_count; i++) if (corr[i].type > 1) return E_INVAL;
        memcpy(d->js_corr, corr, bytes); return 0;
    }
    if (dir == 1 && nr == 0x31) {                                         /* JSIOCSAXMAP: an ABS code per axis, each present */
        uint8_t map[ABS_MAX + 1]; if (size < sizeof(map) || !accessible(argument, sizeof(map), false)) return size < sizeof(map) ? E_INVAL : E_FAULT;
        copy_from_user(map, (const void*)argument, sizeof(map));
        for (unsigned i = 0; i < d->js_axis_count; i++) if (map[i] > ABS_MAX || !evdev_test_bit(d->desc.abs, map[i])) return E_INVAL;
        for (unsigned a = 0; a <= ABS_MAX; a++) d->js_axis_of[a] = -1;
        for (unsigned i = 0; i < d->js_axis_count; i++) { d->js_axes[i] = map[i]; d->js_axis_of[map[i]] = (int8_t)i; }
        return 0;
    }
    if (dir == 1 && nr == 0x33) {                                         /* JSIOCSBTNMAP: a key code per button, each present */
        static uint16_t map[KEY_MAX - BTN_MISC + 1];
        if (size < sizeof(map)) return E_INVAL;
        if (!accessible(argument, sizeof(map), false)) return E_FAULT;
        copy_from_user(map, (const void*)argument, sizeof(map));
        for (unsigned i = 0; i < d->js_button_count; i++) if (map[i] < BTN_MISC || map[i] > KEY_MAX || !evdev_test_bit(d->desc.key, map[i])) return E_INVAL;
        for (unsigned k = 0; k <= KEY_MAX; k++) d->js_button_of[k] = -1;
        for (unsigned i = 0; i < d->js_button_count; i++) { d->js_buttons[i] = map[i]; d->js_button_of[map[i]] = (int16_t)i; }
        return 0;
    }
    return E_NOTTY;
}
static const struct hwnode_ops event_ops = {open_check, event_open, close_common, event_io, event_ioctl, ready_common, source_common, NULL, NULL};
static const struct hwnode_ops js_ops = {open_check, js_open, close_common, js_io, js_ioctl, js_ready, source_common, NULL, NULL};
