/* /dev/uinput (char 10:223): user-space input devices, Linux uinput ABI version 5 - game-controller emulators, remote
 * desktop and test tools.
 *
 * An fd configures a device (UI_SET_EVBIT/KEYBIT/RELBIT/ABSBIT/MSCBIT/LEDBIT/SNDBIT/FFBIT/SWBIT/PROPBIT, UI_SET_PHYS,
 * UI_DEV_SETUP + UI_ABS_SETUP, or the legacy write of struct uinput_user_dev), creates it (UI_DEV_CREATE: an evdev
 * device, + joydev for joysticks, through evdev.c) and then writes input_events into it; UI_GET_SYSNAME names its sysfs
 * object. A keyboard or pointer device also feeds the console's keyboard/pointer nodes (input.c), which is what the
 * desktop reads, so remote-desktop injection reaches Aurora. Force feedback (UI_SET_FFBIT + ff_effects_max): a client's
 * EVIOCSFF/EVIOCRMFF reaches the owner as EV_UINPUT UI_FF_UPLOAD/UI_FF_ERASE with a request id; the client waits
 * (30 s at most) until the owner answers with UI_BEGIN/END_FF_UPLOAD or UI_BEGIN/END_FF_ERASE; effect playback and LED
 * events written by clients are read by the owner. UI_DEV_DESTROY or close removes the device. */
#include "evdev.h"
#include "hwnode.h"
#include "uaccess.h"
#include "../../kernel/string.h"
#include "../../kernel/vga.h"

#define HANDLES 16U
#define OWNER_QUEUE 64U
#define REQUESTS 4U
enum { E_PERM = -1, E_INTR = -4, E_AGAIN = -11, E_NOMEM = -12, E_FAULT = -14, E_BUSY = -16, E_NODEV = -19, E_INVAL = -22, E_NOTTY = -25, E_NOSPC = -28,
       E_TIMEDOUT = -110 };
enum { NEW = 0, SETUP, CREATED };
enum { UI_FF_UPLOAD = 1, UI_FF_ERASE = 2 };
struct input_event64 { int64_t seconds, microseconds; uint16_t type, code; int32_t value; };
struct uinput_setup { uint16_t bustype, vendor, product, version; char name[80]; uint32_t ff_effects_max; };
struct uinput_abs_setup { uint16_t code, pad; struct evdev_absinfo absinfo; };
struct uinput_user_dev { char name[80]; uint16_t bustype, vendor, product, version; uint32_t ff_effects_max; int32_t absmax[64], absmin[64], absfuzz[64], absflat[64]; };
struct uinput_ff_upload { uint32_t request_id; int32_t retval; struct evdev_ff_effect effect, old; };
struct uinput_ff_erase { uint32_t request_id; int32_t retval; uint32_t effect_id; };
_Static_assert(sizeof(struct uinput_setup) == 92 && sizeof(struct uinput_abs_setup) == 28 && sizeof(struct uinput_user_dev) == 1116, "uinput abi");
_Static_assert(sizeof(struct uinput_ff_upload) == 104 && sizeof(struct uinput_ff_erase) == 12, "uinput abi");
struct request { bool used, done, begun; unsigned kind; uint32_t id; int32_t retval; struct evdev_ff_effect effect, old; bool has_old; int effect_id; };
struct handle {
    bool used; unsigned state; uint64_t generation; struct evdev_desc desc; struct evdev* device; bool keyboard, pointer, absolute;
    struct input_event64 queue[OWNER_QUEUE]; unsigned head, count; struct request requests[REQUESTS]; uint32_t next_request;
};
static struct handle* handles;                                          /* HANDLES entries (64 KiB), allocated at boot */
static uint64_t generations;
static int slot = -1;

static struct handle* handle_of(const struct input_reader* reader) {
    unsigned i = reader ? (unsigned)(reader->sequence & 0xFF) : 0;
    if (!handles || !i || i > HANDLES || !handles[i - 1].used || handles[i - 1].generation != reader->sequence >> 8) return NULL;
    return &handles[i - 1];
}
static void owner_push(struct handle* h, unsigned type, unsigned code, int value) {
    uint64_t now = native_realtime_ns();
    if (h->count == OWNER_QUEUE) { h->head = (h->head + 1) % OWNER_QUEUE; h->count--; }   /* the oldest goes */
    h->queue[(h->head + h->count++) % OWNER_QUEUE] = (struct input_event64){(int64_t)(now / 1000000000ULL), (int64_t)(now % 1000000000ULL / 1000), (uint16_t)type, (uint16_t)code, value};
    task_wake_channel(h); event_source_changed(h, 1U);
}
/* ---------- evdev ops: requests to the owner ---------- */
static struct handle* owner_of(struct evdev* device) { return (struct handle*)evdev_context(device); }
static int wait_request(struct handle* h, struct request* r) {
    uint64_t deadline = native_clock_ns() + 30000000000ULL; uint64_t generation = h->generation;
    while (!r->done) {
        if (!h->used || h->generation != generation || h->state != CREATED) return E_NODEV;
        if (native_clock_ns() >= deadline) { r->used = false; return E_TIMEDOUT; }
        int error = task_wait_until(r, native_clock_ns() + 20000000ULL);
        if (error && error != -110) { r->used = false; return error; }
    }
    int result = r->retval; r->used = false;
    return result;
}
static struct request* new_request(struct handle* h, unsigned kind) {
    for (unsigned i = 0; i < REQUESTS; i++) if (!h->requests[i].used) {
        struct request* r = &h->requests[i]; memset(r, 0, sizeof(*r)); r->used = true; r->kind = kind; r->id = ++h->next_request;
        return r;
    }
    return NULL;
}
static int ff_upload(struct evdev* device, struct evdev_ff_effect* effect, const struct evdev_ff_effect* old) {
    struct handle* h = owner_of(device); if (!h) return E_NODEV;
    struct request* r = new_request(h, UI_FF_UPLOAD); if (!r) return E_BUSY;
    r->effect = *effect; if (old) { r->old = *old; r->has_old = true; }
    owner_push(h, EV_UINPUT, UI_FF_UPLOAD, (int)r->id);
    return wait_request(h, r);
}
static int ff_erase(struct evdev* device, int id) {
    struct handle* h = owner_of(device); if (!h) return 0;
    struct request* r = new_request(h, UI_FF_ERASE); if (!r) return E_BUSY;
    r->effect_id = id;
    owner_push(h, EV_UINPUT, UI_FF_ERASE, (int)r->id);
    return wait_request(h, r);
}
static void client_event(struct evdev* device, unsigned type, unsigned code, int value) { struct handle* h = owner_of(device); if (h) owner_push(h, type, code, value); }
static const struct evdev_ops ops = {NULL, ff_upload, ff_erase, client_event};

/* ---------- ioctls ---------- */
static bool get(uint64_t address, void* data, uint64_t size) { if (!accessible(address, size, false)) return false; copy_from_user(data, (const void*)address, size); return true; }
static bool put(uint64_t address, const void* data, uint64_t size) { if (!accessible(address, size, true)) return false; copy_to_user((void*)address, data, size); return true; }
static int set_bit(struct handle* h, unsigned nr, uint64_t value) {
    if (h->state == CREATED) return E_INVAL;
    struct evdev_desc* d = &h->desc;
    switch (nr) {
    case 100: if (value >= 32) return E_INVAL; evdev_set_bit(d->ev, (unsigned)value); return 0;
    case 101: if (value > KEY_MAX) return E_INVAL; evdev_set_bit(d->key, (unsigned)value); return 0;
    case 102: if (value >= 16) return E_INVAL; evdev_set_bit(d->rel, (unsigned)value); return 0;
    case 103: if (value > ABS_MAX) return E_INVAL; evdev_set_bit(d->abs, (unsigned)value); return 0;
    case 104: if (value >= 8) return E_INVAL; evdev_set_bit(d->msc, (unsigned)value); return 0;
    case 105: if (value >= 16) return E_INVAL; evdev_set_bit(d->led, (unsigned)value); return 0;
    case 106: if (value >= 8) return E_INVAL; evdev_set_bit(d->snd, (unsigned)value); return 0;
    case 107: if (value >= 128) return E_INVAL; evdev_set_bit(d->ff, (unsigned)value); return 0;
    case 109: if (value >= 16) return E_INVAL; evdev_set_bit(d->sw, (unsigned)value); return 0;
    case 110: if (value >= 32) return E_INVAL; evdev_set_bit(d->prop, (unsigned)value); return 0;
    }
    return E_NOTTY;
}
static int create(struct handle* h) {
    if (h->state == CREATED) return E_INVAL;
    if (!h->desc.name[0]) return E_INVAL;                               /* UI_DEV_SETUP or the legacy write first */
    struct evdev_desc* d = &h->desc;
    for (unsigned a = 0; a <= ABS_MAX; a++) if (evdev_test_bit(d->abs, a) && d->absinfo[a].maximum < d->absinfo[a].minimum) return E_INVAL;
    if (evdev_test_bit(d->ev, EV_FF) && !d->ff_max) return E_INVAL;
    h->device = evdev_create(d, &ops, h);
    if (!h->device) return E_NOSPC;
    h->state = CREATED;
    for (unsigned k = 1; k < 0x100 && !h->keyboard; k++) h->keyboard = evdev_test_bit(d->key, k);
    h->absolute = evdev_test_bit(d->abs, ABS_X) && evdev_test_bit(d->abs, ABS_Y) && (evdev_test_bit(d->key, 0x110) || evdev_test_bit(d->key, 0x14A)) && !evdev_is_joystick(d);
    h->pointer = h->absolute || ((evdev_test_bit(d->rel, 0) || evdev_test_bit(d->rel, 1)) && evdev_test_bit(d->ev, EV_REL));
    if (h->keyboard) { uint8_t keys[96]; memcpy(keys, d->key, 96); for (unsigned k = 0x100; k < 96 * 8; k++) keys[k / 8] &= (uint8_t)~(1U << (k % 8)); input_source_attach(0, keys, false); }
    if (h->pointer) { uint8_t buttons[96] = {0}; for (unsigned k = 0x110; k < 0x118; k++) if (evdev_test_bit(d->key, k)) evdev_set_bit(buttons, k); input_source_attach(1, buttons, h->absolute); }
    return 0;
}
static void destroy(struct handle* h) {
    if (h->state != CREATED) return;
    for (unsigned i = 0; i < REQUESTS; i++) if (h->requests[i].used) { h->requests[i].done = true; h->requests[i].retval = E_NODEV; task_wake_channel(&h->requests[i]); }
    evdev_destroy(h->device); h->device = NULL; h->state = NEW;
}
static int64_t node_ioctl(unsigned cookie, struct input_reader* reader, uint64_t command, uint64_t argument, uint64_t flags) {
    (void)cookie; (void)flags; struct handle* h = handle_of(reader); if (!h) return E_NODEV;
    unsigned nr = command & 0xFF, type = (command >> 8) & 0xFF, size = (command >> 16) & 0x3FFF, dir = (unsigned)(command >> 30) & 3;
    if (type != 'U') return E_NOTTY;
    if (nr >= 100 && nr <= 110 && nr != 108) { if (dir != 1 || size != 4) return E_NOTTY; return set_bit(h, nr, argument); }
    switch (nr) {
    case 1: if (dir) return E_NOTTY; return create(h);                     /* UI_DEV_CREATE */
    case 2: if (dir) return E_NOTTY; if (h->state != CREATED) return E_INVAL; destroy(h); return 0;   /* UI_DEV_DESTROY */
    case 3: {                                                                /* UI_DEV_SETUP */
        if (dir != 1 || size != sizeof(struct uinput_setup)) return E_NOTTY;
        if (h->state == CREATED) return E_INVAL;
        struct uinput_setup s; if (!get(argument, &s, sizeof(s))) return E_FAULT;
        s.name[79] = 0; if (!s.name[0]) return E_INVAL;
        memcpy(h->desc.name, s.name, 80); h->desc.bustype = s.bustype; h->desc.vendor = s.vendor; h->desc.product = s.product; h->desc.version = s.version;
        h->desc.ff_max = s.ff_effects_max > EVDEV_FF_MAX ? EVDEV_FF_MAX : s.ff_effects_max; h->state = SETUP; return 0;
    }
    case 4: {                                                                /* UI_ABS_SETUP */
        if (dir != 1 || size != sizeof(struct uinput_abs_setup)) return E_NOTTY;
        if (h->state == CREATED) return E_INVAL;
        struct uinput_abs_setup s; if (!get(argument, &s, sizeof(s))) return E_FAULT;
        if (s.code > ABS_MAX || s.absinfo.maximum < s.absinfo.minimum) return E_INVAL;
        h->desc.absinfo[s.code] = s.absinfo; evdev_set_bit(h->desc.abs, s.code); return 0;
    }
    case 44: {                                                               /* UI_GET_SYSNAME(len) */
        if (dir != 2 || h->state != CREATED) return E_INVAL;
        char text[24] = "input";                                       /* the sysfs object inputN of evdev.c */
        unsigned n = (unsigned)evdev_input_number(h->device), at = 5; char digits[12]; unsigned k = 0;
        do { digits[k++] = (char)('0' + n % 10); n /= 10; } while (n);
        while (k) text[at++] = digits[--k];
        text[at++] = 0;
        unsigned length = at < size ? at : size; if (!length) return E_INVAL;
        if (length < at) text[length - 1] = 0;
        return put(argument, text, length) ? (int)length : E_FAULT;
    }
    case 45: { uint32_t version = 5; if (dir != 2) return E_NOTTY; return put(argument, &version, 4) ? 0 : E_FAULT; }   /* UI_GET_VERSION */
    case 108: {                                                              /* UI_SET_PHYS(char*) */
        if (h->state == CREATED) return E_INVAL;
        char phys[64]; uint64_t at = argument; unsigned n = 0;
        for (; n < sizeof(phys) - 1; n++) { char c; if (!get(at + n, &c, 1)) return E_FAULT; phys[n] = c; if (!c) break; }
        phys[n] = 0; memcpy(h->desc.phys, phys, sizeof(phys)); return 0;
    }
    case 200: case 201: case 202: case 203: {                                /* FF upload/erase protocol */
        bool upload = nr <= 201, begin = nr % 2 == 0;
        unsigned length = upload ? sizeof(struct uinput_ff_upload) : sizeof(struct uinput_ff_erase);
        if (size != length) return E_NOTTY;
        static struct uinput_ff_upload u; struct uinput_ff_erase e;
        if (!get(argument, upload ? (void*)&u : (void*)&e, length)) return E_FAULT;
        uint32_t id = upload ? u.request_id : e.request_id; struct request* r = NULL;
        for (unsigned i = 0; i < REQUESTS; i++) if (h->requests[i].used && h->requests[i].id == id && h->requests[i].kind == (upload ? UI_FF_UPLOAD : UI_FF_ERASE) && !h->requests[i].done) r = &h->requests[i];
        if (!r) return E_INVAL;
        if (begin) {
            r->begun = true;
            if (upload) { u.retval = 0; u.effect = r->effect; memset(&u.old, 0, sizeof(u.old)); if (r->has_old) u.old = r->old; return put(argument, &u, length) ? 0 : E_FAULT; }
            e.retval = 0; e.effect_id = (uint32_t)r->effect_id; return put(argument, &e, length) ? 0 : E_FAULT;
        }
        if (!r->begun) return E_INVAL;
        r->retval = upload ? u.retval : e.retval; r->done = true; task_wake_channel(r); return 0;
    }
    }
    return E_NOTTY;
}
static int64_t node_io(unsigned cookie, struct input_reader* reader, uint64_t buffer, uint64_t count, bool write, uint64_t flags) {
    (void)cookie; struct handle* h = handle_of(reader); if (!h) return E_NODEV;
    if (write) {
        if (h->state != CREATED) {                                           /* legacy setup: struct uinput_user_dev */
            if (count != sizeof(struct uinput_user_dev)) return E_INVAL;
            static struct uinput_user_dev u; if (!get(buffer, &u, sizeof(u))) return E_FAULT;
            u.name[79] = 0; if (!u.name[0]) return E_INVAL;
            memcpy(h->desc.name, u.name, 80); h->desc.bustype = u.bustype; h->desc.vendor = u.vendor; h->desc.product = u.product; h->desc.version = u.version;
            h->desc.ff_max = u.ff_effects_max > EVDEV_FF_MAX ? EVDEV_FF_MAX : u.ff_effects_max;
            for (unsigned a = 0; a <= ABS_MAX; a++) {
                h->desc.absinfo[a].minimum = u.absmin[a]; h->desc.absinfo[a].maximum = u.absmax[a]; h->desc.absinfo[a].fuzz = u.absfuzz[a]; h->desc.absinfo[a].flat = u.absflat[a];
                if (evdev_test_bit(h->desc.abs, a) && u.absmax[a] < u.absmin[a]) return E_INVAL;
            }
            h->state = SETUP; return (int64_t)count;
        }
        if (count < sizeof(struct input_event64)) return E_INVAL;
        uint64_t n = count / sizeof(struct input_event64);
        if (!accessible(buffer, n * sizeof(struct input_event64), false)) return E_FAULT;
        for (uint64_t i = 0; i < n; i++) {
            struct input_event64 e; copy_from_user(&e, (const void*)(buffer + i * sizeof(e)), sizeof(e));
            if (h->state != CREATED) return i ? (int64_t)(i * sizeof(e)) : E_NODEV;
            if (e.type == EV_SYN && e.code == SYN_REPORT) {
                evdev_sync(h->device);
                if (h->keyboard) input_source_report(0);
                if (h->pointer) input_source_report(1);
                continue;
            }
            evdev_emit(h->device, e.type, e.code, e.value);
            if (e.type == EV_KEY && h->keyboard && e.code < 0x100) input_source_key(0, e.code, e.value);
            else if (e.type == EV_KEY && h->pointer && e.code >= 0x110 && e.code < 0x118) input_source_key(1, e.code, e.value);
            else if (e.type == EV_REL && h->pointer && (e.code <= 1 || e.code == 6 || e.code == 8)) input_source_motion(EV_REL, e.code, e.value);
            else if (e.type == EV_ABS && h->absolute && e.code <= 1) {      /* to the console pointer's 0..32767 range */
                const struct evdev_absinfo* a = &h->desc.absinfo[e.code]; int64_t range = (int64_t)a->maximum - a->minimum;
                int64_t v = range > 0 ? ((int64_t)e.value - a->minimum) * 32767 / range : 0; if (v < 0) v = 0; if (v > 32767) v = 32767;
                input_source_motion(EV_ABS, e.code, (int)v);
            }
        }
        return (int64_t)(n * sizeof(struct input_event64));
    }
    if (count < sizeof(struct input_event64)) return E_INVAL;
    while (!h->count) {
        if (h->state != CREATED) return E_INVAL;                            /* nothing can arrive before UI_DEV_CREATE */
        if (flags & 0x800) return E_AGAIN;
        int error = task_wait_channel(h); if (error) return error;
        if (!h->used) return E_NODEV;
    }
    uint64_t wanted = count / sizeof(struct input_event64), used = 0;
    if (!accessible(buffer, wanted * sizeof(struct input_event64), true)) return E_FAULT;
    while (used < wanted && h->count) { copy_to_user((void*)(buffer + used * sizeof(struct input_event64)), &h->queue[h->head], sizeof(struct input_event64)); h->head = (h->head + 1) % OWNER_QUEUE; h->count--; used++; }
    return (int64_t)(used * sizeof(struct input_event64));
}
static int open_check(unsigned cookie, uint64_t flags) {
    (void)cookie; (void)flags;
    if (!handles) return -12;
    for (unsigned i = 0; i < HANDLES; i++) if (!handles[i].used) return 0;
    return E_BUSY;
}
static void node_open(unsigned cookie, uint64_t flags, struct input_reader* reader) {
    (void)cookie; (void)flags;
    for (unsigned i = 0; handles && i < HANDLES; i++) if (!handles[i].used) {
        memset(&handles[i], 0, sizeof(handles[i])); handles[i].used = true; handles[i].generation = ++generations;
        reader->sequence = handles[i].generation << 8 | (i + 1); return;
    }
}
static void node_close(unsigned cookie, uint64_t flags, struct input_reader* reader) {
    (void)cookie; (void)flags; struct handle* h = handle_of(reader); if (!h) return;
    destroy(h); h->used = false; reader->sequence = 0; task_wake_channel(h);
}
static uint32_t node_ready(unsigned cookie, const struct input_reader* reader) { (void)cookie; const struct handle* h = handle_of(reader); if (!h) return 8U; return (h->count ? 1U | 64U : 0) | 4U; }
static void* node_source(unsigned cookie, const struct input_reader* reader) { (void)cookie; return (void*)handle_of(reader); }
static const struct hwnode_ops node_ops = {open_check, node_open, node_close, node_io, node_ioctl, node_ready, node_source, NULL, NULL};
void uinput_init(void) {                                                /* boot, before the memory baseline */
    if (!handles) handles = (struct handle*)arena_alloc(HANDLES * sizeof(struct handle));   /* zeroed, never freed (waiters keep pointers) */
    if (slot < 0) slot = hwnode_add("/dev/uinput", 10, 223, 0, 0660, 1000, &node_ops, "misc");
}
