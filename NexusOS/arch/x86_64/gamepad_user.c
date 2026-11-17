/* Game controller input test (tests/test_x64_gamepad.py), built against the real linux/input.h, linux/uinput.h and
   linux/joystick.h (so it also checks the kernel's ABI), run as /bin/hw64 by the /nexus/test-hw campaign.
   Mode = first byte of /nexus/test-hw:
     'u'  uinput -> evdev + joydev: a uinput gamepad (UI_DEV_SETUP/UI_ABS_SETUP, UI_GET_SYSNAME), its sysfs input object
          (name, id, capabilities, uevent with ID_INPUT_JOYSTICK, event/js children, dev numbers), /dev/input discovery,
          the EVIOCG* queries Chrome and SDL make, event delivery with filtering (fuzz, unchanged keys, empty reports),
          clocks, poll, overflow (SYN_DROPPED, joydev state resend), EVIOCGRAB, joydev (init state, button/axis maps,
          default broken-line correction, JSIOCSCORR), force feedback through the uinput upload/erase protocol with a
          second process (upload, update with old effect, rejected upload, play, gain, erase, erase on close),
          uinput keyboard + mouse merged into the console nodes event0/event1, legacy uinput_user_dev setup,
          permissions for an unprivileged user, hostile requests and an ioctl fuzz, UI_DEV_DESTROY with open fds.
     'p'  the virtual Xbox 360 pad of the "vpad" boot option (vpad.c): discovery, layout, its 4-second input pattern
          through evdev and joydev, rumble (the kernel logs it: tests/test_x64_gamepad.py checks the values).
     'n'  nothing: no joystick nodes without vpad, /dev/uinput present. */
#define _GNU_SOURCE
#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <poll.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>
#include <sys/ioctl.h>
#include <sys/stat.h>
#include <sys/sysmacros.h>
#include <sys/wait.h>
#include <grp.h>
#include <linux/input.h>
#include <linux/joystick.h>
#include <linux/uinput.h>

static int failures;
#define CHECK(test, ...) do { if (!(test)) { printf("[64] GAMEPAD FAIL line=%d: ", __LINE__); printf(__VA_ARGS__); printf(" (errno %d)\n", errno); fflush(stdout); failures++; } } while (0)
static void sleep_ms(int ms) { struct timespec t = {ms / 1000, (long)(ms % 1000) * 1000000}; nanosleep(&t, NULL); }
static double now_s(clockid_t clock) { struct timespec t; clock_gettime(clock, &t); return (double)t.tv_sec + t.tv_nsec / 1e9; }
static double absd(double v) { return v < 0 ? -v : v; }
static bool test_bit(const unsigned char* bits, unsigned bit) { return (bits[bit / 8] >> (bit % 8)) & 1; }
static int emit(int fd, unsigned type, unsigned code, int value) {
    struct input_event e; memset(&e, 0, sizeof(e)); e.type = (unsigned short)type; e.code = (unsigned short)code; e.value = value;
    return write(fd, &e, sizeof(e)) == (ssize_t)sizeof(e) ? 0 : -1;
}
static int read_events(int fd, struct input_event* out, int max) {
    ssize_t n = read(fd, out, sizeof(*out) * (size_t)max);
    return n < 0 ? -1 : (int)(n / (ssize_t)sizeof(*out));
}
static int read_js(int fd, struct js_event* out, int max) {
    ssize_t n = read(fd, out, sizeof(*out) * (size_t)max);
    return n < 0 ? -1 : (int)(n / (ssize_t)sizeof(*out));
}
static int read_text(const char* path, char* out, size_t size) {
    int fd = open(path, O_RDONLY); if (fd < 0) return -1;
    ssize_t n = read(fd, out, size - 1); close(fd);
    if (n < 0) return -1;
    out[n] = 0; return (int)n;
}
/* /dev/input/<prefix>N whose name (EVIOCGNAME / JSIOCGNAME) is name; -1 if none. */
static int find_node(const char* prefix, const char* name, char* path, size_t size) {
    DIR* dir = opendir("/dev/input"); int found = -1;
    for (struct dirent* e = dir ? readdir(dir) : NULL; e && found < 0; e = readdir(dir)) {
        if (strncmp(e->d_name, prefix, strlen(prefix))) continue;
        char candidate[64]; snprintf(candidate, sizeof(candidate), "/dev/input/%s", e->d_name);
        int fd = open(candidate, O_RDONLY | O_NONBLOCK); if (fd < 0) continue;
        char text[128] = {0};
        int r = !strcmp(prefix, "js") ? ioctl(fd, JSIOCGNAME(sizeof(text)), text) : ioctl(fd, EVIOCGNAME(sizeof(text)), text);
        if (r > 0 && !strcmp(text, name)) { found = atoi(e->d_name + strlen(prefix)); snprintf(path, size, "%s", candidate); }
        close(fd);
    }
    if (dir) closedir(dir);
    return found;
}
static int count_nodes(const char* prefix) {
    DIR* dir = opendir("/dev/input"); int count = 0;
    for (struct dirent* e = dir ? readdir(dir) : NULL; e; e = readdir(dir)) if (!strncmp(e->d_name, prefix, strlen(prefix))) count++;
    if (dir) closedir(dir);
    return count;
}
static int set_bits(int u, unsigned long request, const unsigned* codes, int n) {
    for (int i = 0; i < n; i++) if (ioctl(u, request, codes[i]) < 0) return -1;
    return 0;
}
/* joydev's default correction for an axis (drivers/input/joydev.c), to check the reported values */
static int js_expected(int value, int minimum, int maximum, int flat) {
    long long center = ((long long)maximum + minimum) / 2, c0 = center - flat, c1 = center + flat, t = ((long long)maximum - minimum) / 2 - 2LL * flat;
    long long c2 = t ? (1LL << 29) / t : 0, v = value;
    v = v > c0 ? (v < c1 ? 0 : (c2 * (v - c1)) >> 14) : (c2 * (v - c0)) >> 14;
    return (int)(v > 32767 ? 32767 : v < -32767 ? -32767 : v);
}

#define PAD_NAME "NexusOS Test Pad"
static const unsigned PAD_KEYS[] = {BTN_SOUTH, BTN_EAST, BTN_NORTH, BTN_WEST, BTN_TL, BTN_TR, BTN_SELECT, BTN_START, BTN_MODE, BTN_THUMBL, BTN_THUMBR, BTN_DPAD_UP};
static const unsigned JS_ORDER[] = {BTN_SOUTH, BTN_EAST, BTN_NORTH, BTN_WEST, BTN_TL, BTN_TR, BTN_SELECT, BTN_START, BTN_MODE, BTN_THUMBL, BTN_THUMBR, BTN_DPAD_UP};
static int create_pad(void) {
    int u = open("/dev/uinput", O_RDWR | O_NONBLOCK);
    CHECK(u >= 0, "open /dev/uinput");
    if (u < 0) return -1;
    unsigned version = 0;
    CHECK(!ioctl(u, UI_GET_VERSION, &version) && version >= 5, "UI_GET_VERSION %u", version);
    /* hostile setup */
    errno = 0; CHECK(ioctl(u, UI_DEV_CREATE) < 0 && errno == EINVAL, "create without a name");
    struct input_event e; errno = 0; CHECK(read(u, &e, sizeof(e)) < 0 && errno == EINVAL, "read before create");
    errno = 0; CHECK(ioctl(u, UI_SET_KEYBIT, KEY_MAX + 1) < 0 && errno == EINVAL, "key bit beyond KEY_MAX");
    errno = 0; CHECK(ioctl(u, UI_SET_EVBIT, 32) < 0 && errno == EINVAL, "event type 32");
    errno = 0; CHECK(ioctl(u, UI_SET_ABSBIT, ABS_MAX + 1) < 0 && errno == EINVAL, "abs bit beyond ABS_MAX");
    struct uinput_abs_setup bad = {.code = ABS_MAX + 1}; errno = 0; CHECK(ioctl(u, UI_ABS_SETUP, &bad) < 0 && errno == EINVAL, "abs setup code");
    bad = (struct uinput_abs_setup){.code = ABS_X, .absinfo = {.minimum = 10, .maximum = -10}}; errno = 0; CHECK(ioctl(u, UI_ABS_SETUP, &bad) < 0 && errno == EINVAL, "abs max < min");
    errno = 0; CHECK(ioctl(u, UI_DEV_SETUP, NULL) < 0 && errno == EFAULT, "setup from NULL");
    struct uinput_setup empty; memset(&empty, 0, sizeof(empty)); errno = 0; CHECK(ioctl(u, UI_DEV_SETUP, &empty) < 0 && errno == EINVAL, "empty name");
    char sysname[64]; errno = 0; CHECK(ioctl(u, UI_GET_SYSNAME(sizeof(sysname)), sysname) < 0, "sysname before create");
    errno = 0; CHECK(ioctl(u, _IOW('U', 99, int), 0) < 0 && errno == ENOTTY, "unknown uinput ioctl");
    /* the pad */
    unsigned types[] = {EV_KEY, EV_ABS, EV_FF};
    unsigned ff[] = {FF_RUMBLE, FF_PERIODIC, FF_SINE, FF_GAIN};
    CHECK(!set_bits(u, UI_SET_EVBIT, types, 3) && !set_bits(u, UI_SET_KEYBIT, PAD_KEYS, 12) && !set_bits(u, UI_SET_FFBIT, ff, 4), "capability bits");
    struct uinput_abs_setup axes[] = {{ABS_X, {0, -32768, 32767, 16, 128, 0}}, {ABS_Y, {0, -32768, 32767, 16, 128, 0}},
                                      {ABS_Z, {0, 0, 255, 0, 0, 0}}, {ABS_HAT0X, {0, -1, 1, 0, 0, 0}}};
    for (int i = 0; i < 4; i++) CHECK(!ioctl(u, UI_ABS_SETUP, &axes[i]), "UI_ABS_SETUP %d", i);
    struct uinput_setup setup; memset(&setup, 0, sizeof(setup));
    setup.id = (struct input_id){BUS_USB, 0x1234, 0x5678, 1}; strcpy(setup.name, PAD_NAME); setup.ff_effects_max = 4;
    CHECK(!ioctl(u, UI_DEV_SETUP, &setup), "UI_DEV_SETUP");
    CHECK(!ioctl(u, UI_SET_PHYS, "nexus-test/input0"), "UI_SET_PHYS");
    CHECK(!ioctl(u, UI_DEV_CREATE), "UI_DEV_CREATE");
    errno = 0; CHECK(ioctl(u, UI_DEV_CREATE) < 0 && errno == EINVAL, "second create");
    errno = 0; CHECK(ioctl(u, UI_SET_KEYBIT, BTN_C) < 0 && errno == EINVAL, "bits after create");
    return u;
}

static void check_sysfs(int u, int event, int js) {
    char sysname[64] = {0};
    int n = ioctl(u, UI_GET_SYSNAME(sizeof(sysname)), sysname);
    CHECK(n > 0 && !strncmp(sysname, "input", 5), "UI_GET_SYSNAME %d '%s'", n, sysname);
    char path[160], text[2048];
    snprintf(path, sizeof(path), "/sys/class/input/%s/name", sysname);
    CHECK(read_text(path, text, sizeof(text)) > 0 && !strcmp(text, PAD_NAME "\n"), "sysfs name '%s'", text);
    snprintf(path, sizeof(path), "/sys/class/input/%s/id/vendor", sysname);
    CHECK(read_text(path, text, sizeof(text)) > 0 && !strcmp(text, "1234\n"), "sysfs vendor '%s'", text);
    snprintf(path, sizeof(path), "/sys/class/input/%s/id/product", sysname);
    CHECK(read_text(path, text, sizeof(text)) > 0 && !strcmp(text, "5678\n"), "sysfs product '%s'", text);
    snprintf(path, sizeof(path), "/sys/class/input/%s/phys", sysname);
    CHECK(read_text(path, text, sizeof(text)) > 0 && !strcmp(text, "nexus-test/input0\n"), "sysfs phys '%s'", text);
    snprintf(path, sizeof(path), "/sys/class/input/%s/capabilities/ev", sysname);
    CHECK(read_text(path, text, sizeof(text)) > 0 && strtoul(text, NULL, 16) == ((1UL << EV_SYN) | (1UL << EV_KEY) | (1UL << EV_ABS) | (1UL << EV_FF)), "capabilities/ev '%s'", text);
    snprintf(path, sizeof(path), "/sys/class/input/%s/capabilities/abs", sysname);
    CHECK(read_text(path, text, sizeof(text)) > 0 && strtoull(text, NULL, 16) == ((1ULL << ABS_X) | (1ULL << ABS_Y) | (1ULL << ABS_Z) | (1ULL << ABS_HAT0X)), "capabilities/abs '%s'", text);
    snprintf(path, sizeof(path), "/sys/class/input/%s/uevent", sysname);
    CHECK(read_text(path, text, sizeof(text)) > 0 && strstr(text, "ID_INPUT_JOYSTICK=1") && strstr(text, "PRODUCT=3/1234/5678/1") && strstr(text, "NAME=\"" PAD_NAME "\""),
          "input uevent '%s'", text);
    snprintf(path, sizeof(path), "/sys/class/input/%s/modalias", sysname);
    CHECK(read_text(path, text, sizeof(text)) > 0 && !strncmp(text, "input:b0003v1234p5678e0001-e0,1,3,15,", 37), "modalias '%s'", text);
    /* children: eventN, jsN, also reachable as /sys/class/input/eventN */
    bool saw_event = false, saw_js = false;
    snprintf(path, sizeof(path), "/sys/class/input/%s", sysname);
    DIR* dir = opendir(path);
    CHECK(dir != NULL, "opendir %s", path);
    for (struct dirent* e = dir ? readdir(dir) : NULL; e; e = readdir(dir)) {
        char want[32]; snprintf(want, sizeof(want), "event%d", event); saw_event |= !strcmp(e->d_name, want);
        snprintf(want, sizeof(want), "js%d", js); saw_js |= !strcmp(e->d_name, want);
    }
    if (dir) closedir(dir);
    CHECK(saw_event && saw_js, "children event%d js%d listed", event, js);
    snprintf(path, sizeof(path), "/sys/class/input/event%d/dev", event);
    char want[32]; snprintf(want, sizeof(want), "13:%d\n", 64 + event);
    CHECK(read_text(path, text, sizeof(text)) > 0 && !strcmp(text, want), "event dev '%s'", text);
    snprintf(path, sizeof(path), "/sys/class/input/js%d/uevent", js);
    snprintf(want, sizeof(want), "DEVNAME=input/js%d", js);
    CHECK(read_text(path, text, sizeof(text)) > 0 && strstr(text, want) && strstr(text, "MAJOR=13") && strstr(text, "ID_INPUT_JOYSTICK=1"), "js uevent '%s'", text);
    snprintf(path, sizeof(path), "/sys/class/input/event%d/device/name", event);
    CHECK(read_text(path, text, sizeof(text)) > 0 && !strcmp(text, PAD_NAME "\n"), "event device link name '%s'", text);
    struct stat st; snprintf(path, sizeof(path), "/dev/input/event%d", event);
    CHECK(!stat(path, &st) && S_ISCHR(st.st_mode) && major(st.st_rdev) == 13 && minor(st.st_rdev) == (unsigned)(64 + event), "stat %s", path);
    snprintf(path, sizeof(path), "/dev/input/js%d", js);
    CHECK(!stat(path, &st) && S_ISCHR(st.st_mode) && major(st.st_rdev) == 13 && minor(st.st_rdev) == (unsigned)js, "stat %s", path);
}

static void check_queries(int fd) {
    int version = 0; CHECK(!ioctl(fd, EVIOCGVERSION, &version) && version == 0x010001, "EVIOCGVERSION %x", version);
    struct input_id id; CHECK(!ioctl(fd, EVIOCGID, &id) && id.bustype == BUS_USB && id.vendor == 0x1234 && id.product == 0x5678 && id.version == 1, "EVIOCGID");
    char name[256] = {0}; int n = ioctl(fd, EVIOCGNAME(sizeof(name)), name);
    CHECK(n == (int)strlen(PAD_NAME) + 1 && !strcmp(name, PAD_NAME), "EVIOCGNAME %d", n);
    char shortname[4]; n = ioctl(fd, EVIOCGNAME(sizeof(shortname)), shortname); CHECK(n == 4 && !memcmp(shortname, "Nexu", 4), "EVIOCGNAME(4) %d", n);
    char phys[64] = {0}; n = ioctl(fd, EVIOCGPHYS(sizeof(phys)), phys); CHECK(n > 0 && !strcmp(phys, "nexus-test/input0"), "EVIOCGPHYS '%s'", phys);
    errno = 0; CHECK(ioctl(fd, EVIOCGUNIQ(sizeof(phys)), phys) < 0 && errno == ENOENT, "EVIOCGUNIQ without uniq");
    unsigned char ev[EV_MAX / 8 + 1] = {0}; CHECK(ioctl(fd, EVIOCGBIT(0, sizeof(ev)), ev) >= 0 && test_bit(ev, EV_SYN) && test_bit(ev, EV_KEY) && test_bit(ev, EV_ABS) && test_bit(ev, EV_FF) && !test_bit(ev, EV_REL), "EVIOCGBIT(0)");
    unsigned char key[KEY_MAX / 8 + 1] = {0}; CHECK(ioctl(fd, EVIOCGBIT(EV_KEY, sizeof(key)), key) >= 0, "EVIOCGBIT(EV_KEY)");
    for (int i = 0; i < 12; i++) CHECK(test_bit(key, PAD_KEYS[i]), "key bit %x", PAD_KEYS[i]);
    CHECK(!test_bit(key, BTN_C) && !test_bit(key, KEY_A), "no extra keys");
    unsigned char abs[ABS_MAX / 8 + 1] = {0}; CHECK(ioctl(fd, EVIOCGBIT(EV_ABS, sizeof(abs)), abs) >= 0 && test_bit(abs, ABS_X) && test_bit(abs, ABS_HAT0X) && !test_bit(abs, ABS_RX), "EVIOCGBIT(EV_ABS)");
    unsigned char ffb[FF_MAX / 8 + 1] = {0}; CHECK(ioctl(fd, EVIOCGBIT(EV_FF, sizeof(ffb)), ffb) >= 0 && test_bit(ffb, FF_RUMBLE) && test_bit(ffb, FF_SINE) && test_bit(ffb, FF_GAIN), "EVIOCGBIT(EV_FF)");
    unsigned char rel[REL_MAX / 8 + 1] = {0xFF, 0xFF}; CHECK(ioctl(fd, EVIOCGBIT(EV_REL, sizeof(rel)), rel) >= 0 && !rel[0] && !rel[1], "EVIOCGBIT(EV_REL) empty");
    struct input_absinfo a; CHECK(!ioctl(fd, EVIOCGABS(ABS_X), &a) && a.minimum == -32768 && a.maximum == 32767 && a.fuzz == 16 && a.flat == 128, "EVIOCGABS(ABS_X)");
    CHECK(!ioctl(fd, EVIOCGABS(ABS_HAT0X), &a) && a.minimum == -1 && a.maximum == 1, "EVIOCGABS(ABS_HAT0X)");
    int effects = 0; CHECK(!ioctl(fd, EVIOCGEFFECTS, &effects) && effects == 4, "EVIOCGEFFECTS %d", effects);
    unsigned char prop[INPUT_PROP_MAX / 8 + 1] = {0xFF}; CHECK(ioctl(fd, EVIOCGPROP(sizeof(prop)), prop) >= 0 && !prop[0], "EVIOCGPROP");
    unsigned char state[KEY_MAX / 8 + 1]; memset(state, 0xFF, sizeof(state)); CHECK(ioctl(fd, EVIOCGKEY(sizeof(state)), state) >= 0 && !test_bit(state, BTN_SOUTH), "EVIOCGKEY");
    /* hostile */
    errno = 0; CHECK(ioctl(fd, EVIOCGABS(ABS_RX), &a) < 0 && errno == EINVAL, "EVIOCGABS of a missing axis");
    errno = 0; CHECK(ioctl(fd, EVIOCGABS(ABS_X), (void*)8) < 0 && errno == EFAULT, "EVIOCGABS to a bad pointer");
    errno = 0; CHECK(ioctl(fd, EVIOCGNAME(256), NULL) < 0 && errno == EFAULT, "EVIOCGNAME to NULL");
    errno = 0; CHECK(ioctl(fd, EVIOCGBIT(0x1F, 8), ev) < 0 && errno == EINVAL, "EVIOCGBIT of type 0x1f");
    int clock = 5; errno = 0; CHECK(ioctl(fd, EVIOCSCLOCKID, &clock) < 0 && errno == EINVAL, "EVIOCSCLOCKID 5");
    errno = 0; CHECK(ioctl(fd, EVIOCRMFF, 99) < 0 && errno == EINVAL, "EVIOCRMFF 99");
    struct ff_effect fx; memset(&fx, 0, sizeof(fx)); fx.type = 0; fx.id = -1; errno = 0; CHECK(ioctl(fd, EVIOCSFF, &fx) < 0 && errno == EINVAL, "EVIOCSFF type 0");
    fx.type = FF_CONSTANT; errno = 0; CHECK(ioctl(fd, EVIOCSFF, &fx) < 0 && errno == EINVAL, "EVIOCSFF of an unsupported type");
    fx.type = FF_PERIODIC; fx.u.periodic.waveform = FF_SQUARE; errno = 0; CHECK(ioctl(fd, EVIOCSFF, &fx) < 0 && errno == EINVAL, "EVIOCSFF of an unsupported waveform");
    fx.type = FF_RUMBLE; fx.id = 3; errno = 0; CHECK(ioctl(fd, EVIOCSFF, &fx) < 0 && errno == EINVAL, "EVIOCSFF update of an unused id");
    errno = 0; CHECK(ioctl(fd, EVIOCSFF, (void*)16) < 0 && errno == EFAULT, "EVIOCSFF from a bad pointer");
    struct input_event e; errno = 0; CHECK(read(fd, &e, 8) < 0 && errno == EINVAL, "short read");
    errno = 0; CHECK(read(fd, (void*)24, sizeof(e)) < 0, "read to a bad pointer");
}

/* ---------- force feedback through the uinput protocol (two processes) ---------- */
struct ff_counts { int uploads, erases, plays, gains, rejected, updates_with_old; };
static void ff_service(int u, struct ff_counts* c) {
    struct input_event e[16];
    for (int n = read_events(u, e, 16), i = 0; n > 0 && i < n; i++) {
        if (e[i].type == EV_UINPUT && e[i].code == UI_FF_UPLOAD) {
            struct uinput_ff_upload up; memset(&up, 0, sizeof(up)); up.request_id = (unsigned)e[i].value;
            CHECK(!ioctl(u, UI_BEGIN_FF_UPLOAD, &up), "UI_BEGIN_FF_UPLOAD");
            c->uploads++;
            if (up.effect.type == FF_RUMBLE && up.effect.u.rumble.strong_magnitude == 0x1111) { up.retval = -EINVAL; c->rejected++; }
            else up.retval = 0;
            if (up.old.type == FF_RUMBLE && up.old.u.rumble.strong_magnitude == 0x8000 && up.effect.u.rumble.strong_magnitude == 0x9000) c->updates_with_old++;
            errno = 0; struct uinput_ff_upload wrong = up; wrong.request_id += 1000;
            CHECK(ioctl(u, UI_END_FF_UPLOAD, &wrong) < 0 && errno == EINVAL, "end of an unknown request");
            CHECK(!ioctl(u, UI_END_FF_UPLOAD, &up), "UI_END_FF_UPLOAD");
        } else if (e[i].type == EV_UINPUT && e[i].code == UI_FF_ERASE) {
            struct uinput_ff_erase er; memset(&er, 0, sizeof(er)); er.request_id = (unsigned)e[i].value;
            CHECK(!ioctl(u, UI_BEGIN_FF_ERASE, &er), "UI_BEGIN_FF_ERASE");
            er.retval = 0; CHECK(!ioctl(u, UI_END_FF_ERASE, &er), "UI_END_FF_ERASE");
            c->erases++;
        } else if (e[i].type == EV_FF && e[i].code == FF_GAIN) c->gains++;
        else if (e[i].type == EV_FF && e[i].value == 1) c->plays++;
    }
}
static int ff_child(const char* event_path) {
    int fd = open(event_path, O_RDWR); if (fd < 0) return 10;
    struct ff_effect fx; memset(&fx, 0, sizeof(fx));
    fx.type = FF_RUMBLE; fx.id = -1; fx.u.rumble.strong_magnitude = 0x8000; fx.u.rumble.weak_magnitude = 0x4000; fx.replay.length = 500;
    if (ioctl(fd, EVIOCSFF, &fx) < 0 || fx.id < 0) return 11;
    int id = fx.id;
    if (emit(fd, EV_FF, (unsigned)id, 1) < 0) return 12;
    if (emit(fd, EV_FF, FF_GAIN, 0x8000) < 0) return 13;
    fx.u.rumble.strong_magnitude = 0x9000;
    if (ioctl(fd, EVIOCSFF, &fx) < 0 || fx.id != id) return 14;          /* update: the owner sees the old effect */
    struct ff_effect rejected; memset(&rejected, 0, sizeof(rejected)); rejected.type = FF_RUMBLE; rejected.id = -1; rejected.u.rumble.strong_magnitude = 0x1111;
    errno = 0; if (ioctl(fd, EVIOCSFF, &rejected) == 0 || errno != EINVAL) return 15;
    struct ff_effect sine; memset(&sine, 0, sizeof(sine)); sine.type = FF_PERIODIC; sine.id = -1; sine.u.periodic.waveform = FF_SINE; sine.u.periodic.magnitude = 0x2000; sine.u.periodic.period = 100;
    if (ioctl(fd, EVIOCSFF, &sine) < 0 || sine.id < 0 || sine.id == id) return 16;
    if (ioctl(fd, EVIOCRMFF, id) < 0) return 17;
    errno = 0; if (ioctl(fd, EVIOCRMFF, id) == 0 || errno != EINVAL) return 18;
    close(fd);                                                            /* the sine effect is erased by the close */
    return 0;
}
static void check_force_feedback(int u, const char* event_path) {
    pid_t child = fork();
    if (!child) _exit(ff_child(event_path));
    CHECK(child > 0, "fork");
    if (child <= 0) return;
    struct ff_counts c = {0}; int status = -1; double deadline = now_s(CLOCK_MONOTONIC) + 20;
    while (now_s(CLOCK_MONOTONIC) < deadline) {
        struct pollfd p = {u, POLLIN, 0}; poll(&p, 1, 50);
        ff_service(u, &c);
        pid_t done = waitpid(child, &status, WNOHANG);
        if (done == child) { ff_service(u, &c); break; }
    }
    CHECK(WIFEXITED(status) && WEXITSTATUS(status) == 0, "force feedback client: status %x", status);
    CHECK(c.uploads == 4 && c.rejected == 1 && c.updates_with_old == 1 && c.erases == 2 && c.plays >= 1 && c.gains == 1,
          "owner saw uploads=%d rejected=%d with-old=%d erases=%d plays=%d gains=%d", c.uploads, c.rejected, c.updates_with_old, c.erases, c.plays, c.gains);
    printf("[64] GAMEPAD ff: uploads=%d rejected=%d erases=%d plays=%d gains=%d\n", c.uploads, c.rejected, c.erases, c.plays, c.gains);
}

/* ---------- console merging: a uinput keyboard and mouse reach event0/event1 ---------- */
static void check_console_merge(void) {
    int k = open("/dev/uinput", O_RDWR | O_NONBLOCK); CHECK(k >= 0, "open uinput (keyboard)");
    if (k < 0) return;
    unsigned keys[] = {KEY_A, KEY_ENTER, KEY_LEFTSHIFT, BTN_LEFT, BTN_RIGHT}, types[] = {EV_KEY, EV_REL}, rels[] = {REL_X, REL_Y, REL_WHEEL};
    CHECK(!set_bits(k, UI_SET_EVBIT, types, 2) && !set_bits(k, UI_SET_KEYBIT, keys, 5) && !set_bits(k, UI_SET_RELBIT, rels, 3), "keyboard bits");
    struct uinput_setup s; memset(&s, 0, sizeof(s)); s.id = (struct input_id){BUS_VIRTUAL, 1, 2, 3}; strcpy(s.name, "NexusOS Test Keyboard");
    CHECK(!ioctl(k, UI_DEV_SETUP, &s) && !ioctl(k, UI_DEV_CREATE), "keyboard create");
    char path[64]; int event = find_node("event", "NexusOS Test Keyboard", path, sizeof(path));
    CHECK(event >= 2, "keyboard event node %d", event);
    CHECK(find_node("js", "NexusOS Test Keyboard", path, sizeof(path)) < 0, "no joydev node for a keyboard/mouse");
    int kbd = open("/dev/input/event0", O_RDONLY | O_NONBLOCK), mouse = open("/dev/input/event1", O_RDONLY | O_NONBLOCK);
    CHECK(kbd >= 0 && mouse >= 0, "console nodes");
    struct input_event drain[64]; while (kbd >= 0 && read_events(kbd, drain, 64) > 0) {} while (mouse >= 0 && read_events(mouse, drain, 64) > 0) {}
    emit(k, EV_KEY, KEY_A, 1); emit(k, EV_SYN, SYN_REPORT, 0); emit(k, EV_KEY, KEY_A, 0); emit(k, EV_SYN, SYN_REPORT, 0);
    emit(k, EV_REL, REL_X, 7); emit(k, EV_REL, REL_Y, -3); emit(k, EV_SYN, SYN_REPORT, 0);
    sleep_ms(20);
    struct input_event got[64]; int n = kbd >= 0 ? read_events(kbd, got, 64) : -1, down = 0, up = 0;
    for (int i = 0; i < n; i++) if (got[i].type == EV_KEY && got[i].code == KEY_A) { if (got[i].value == 1) down++; else if (!got[i].value) up++; }
    CHECK(down == 1 && up == 1, "KEY_A on event0: %d events, down=%d up=%d", n, down, up);
    n = mouse >= 0 ? read_events(mouse, got, 64) : -1; int dx = 0, dy = 0;
    for (int i = 0; i < n; i++) if (got[i].type == EV_REL) { if (got[i].code == REL_X) dx += got[i].value; if (got[i].code == REL_Y) dy += got[i].value; }
    CHECK(dx == 7 && dy == -3, "pointer motion on event1: dx=%d dy=%d", dx, dy);
    if (kbd >= 0) close(kbd);
    if (mouse >= 0) close(mouse);
    CHECK(!ioctl(k, UI_DEV_DESTROY), "keyboard destroy");
    close(k);
    /* legacy setup: uinput_user_dev written before UI_DEV_CREATE */
    int l = open("/dev/uinput", O_WRONLY); CHECK(l >= 0, "open uinput (legacy)");
    if (l < 0) return;
    CHECK(!ioctl(l, UI_SET_EVBIT, EV_ABS) && !ioctl(l, UI_SET_ABSBIT, ABS_X) && !ioctl(l, UI_SET_ABSBIT, ABS_Y) && !ioctl(l, UI_SET_EVBIT, EV_KEY) && !ioctl(l, UI_SET_KEYBIT, BTN_TRIGGER), "legacy bits");
    struct uinput_user_dev legacy; memset(&legacy, 0, sizeof(legacy)); strcpy(legacy.name, "NexusOS Legacy Stick"); legacy.id.bustype = BUS_VIRTUAL;
    legacy.absmin[ABS_X] = legacy.absmin[ABS_Y] = 0; legacy.absmax[ABS_X] = legacy.absmax[ABS_Y] = 1023; legacy.absflat[ABS_X] = 8;
    errno = 0; CHECK(write(l, &legacy, sizeof(legacy) - 1) < 0 && errno == EINVAL, "legacy setup of the wrong size");
    CHECK(write(l, &legacy, sizeof(legacy)) == (ssize_t)sizeof(legacy) && !ioctl(l, UI_DEV_CREATE), "legacy setup + create");
    int stick = find_node("event", "NexusOS Legacy Stick", path, sizeof(path)), fd = stick >= 0 ? open(path, O_RDONLY) : -1;
    struct input_absinfo a = {0};
    CHECK(fd >= 0 && !ioctl(fd, EVIOCGABS(ABS_X), &a) && a.maximum == 1023 && a.flat == 8, "legacy axis range %d", a.maximum);
    CHECK(find_node("js", "NexusOS Legacy Stick", path, sizeof(path)) >= 0, "joystick (BTN_TRIGGER) gets a js node");
    if (fd >= 0) close(fd);
    close(l);                                                              /* close destroys the device */
    CHECK(find_node("event", "NexusOS Legacy Stick", path, sizeof(path)) < 0, "device gone after close");
}

/* ---------- unprivileged users (event nodes 0660 uid 1000 like the logind seat user, uinput 0660) ---------- */
static void check_permissions(const char* event_path) {
    struct stat st;
    CHECK(!stat("/dev/uinput", &st) && (st.st_mode & 07777) == 0660 && st.st_uid == 1000, "/dev/uinput mode %o uid %d", st.st_mode & 07777, (int)st.st_uid);
    CHECK(!stat(event_path, &st) && (st.st_mode & 07777) == 0660 && st.st_uid == 1000, "%s mode %o uid %d", event_path, st.st_mode & 07777, (int)st.st_uid);
    if (getuid() != 0) { printf("[64] GAMEPAD permissions: running as uid %d (the seat user): modes checked, no other user to switch to\n", (int)getuid()); return; }
    pid_t child = fork();
    if (!child) {
        if (setgroups(0, NULL)) _exit(101);
        if (setresgid(65534, 65534, 65534)) _exit(102);
        if (setresuid(65534, 65534, 65534)) _exit(103);
        if (open("/dev/uinput", O_RDWR) >= 0 || errno != EACCES) _exit(21);
        if (open(event_path, O_RDONLY) >= 0 || errno != EACCES) _exit(22);
        _exit(0);
    }
    int status = -1; waitpid(child, &status, 0);
    CHECK(WIFEXITED(status) && WEXITSTATUS(status) == 0, "nobody cannot open uinput/event nodes: status %x", status);
}

/* ---------- hostile ioctl fuzz: read-direction commands only (no grabs, revokes or uploads) ---------- */
static uint32_t lcg = 12345;
static uint32_t random32(void) { lcg = lcg * 1103515245U + 12345U; return lcg >> 1; }
static void fuzz(int event_fd, int js_fd, int u_fd) {
    static unsigned char scratch[8192];
    for (int i = 0; i < 4000; i++) {
        unsigned type = (const unsigned char[]){'E', 'j', 'U', 'E', 'j', 'X'}[random32() % 6], nr = random32() & 0xFF, size = random32() % 4097;
        int fd = type == 'E' ? event_fd : type == 'j' ? js_fd : type == 'U' ? u_fd : (random32() & 1 ? event_fd : js_fd);
        unsigned dir = 2;                                                  /* _IOC_READ */
        if (type == 'U' && (nr == 1 || nr == 2 || (nr >= 200 && nr <= 203))) continue;   /* create/destroy and the ff protocol */
        unsigned long command = ((unsigned long)dir << 30) | ((unsigned long)size << 16) | (type << 8) | nr;
        void* argument = random32() % 8 ? (void*)scratch : (void*)(uintptr_t)(random32() % 4096);
        ioctl(fd, command, argument);
    }
    int version = 0; CHECK(!ioctl(event_fd, EVIOCGVERSION, &version) && version == 0x010001, "event fd alive after the fuzz");
    CHECK(!ioctl(js_fd, JSIOCGVERSION, &version) && version == 0x020100, "js fd alive after the fuzz");
}

static void campaign_uinput(void) {
    int u = create_pad(); if (u < 0) return;
    char event_path[64], js_path[64];
    int event = find_node("event", PAD_NAME, event_path, sizeof(event_path)), js = find_node("js", PAD_NAME, js_path, sizeof(js_path));
    CHECK(event >= 2 && js >= 0, "nodes event%d js%d", event, js);
    if (event < 0 || js < 0) return;
    printf("[64] GAMEPAD uinput pad: %s %s\n", event_path, js_path);
    check_sysfs(u, event, js);
    int e1 = open(event_path, O_RDONLY | O_NONBLOCK), e2 = open(event_path, O_RDWR | O_NONBLOCK), j = open(js_path, O_RDONLY | O_NONBLOCK);
    CHECK(e1 >= 0 && e2 >= 0 && j >= 0, "open nodes");
    if (e1 < 0 || e2 < 0 || j < 0) return;
    check_queries(e1);
    /* joydev identity and maps */
    unsigned version = 0; unsigned char axes = 0, buttons = 0; char name[128] = {0};
    CHECK(!ioctl(j, JSIOCGVERSION, &version) && version == JS_VERSION, "JSIOCGVERSION %x", version);
    CHECK(!ioctl(j, JSIOCGAXES, &axes) && axes == 4, "JSIOCGAXES %d", axes);
    CHECK(!ioctl(j, JSIOCGBUTTONS, &buttons) && buttons == 12, "JSIOCGBUTTONS %d", buttons);
    CHECK(ioctl(j, JSIOCGNAME(sizeof(name)), name) > 0 && !strcmp(name, PAD_NAME), "JSIOCGNAME");
    unsigned char axmap[ABS_CNT] = {0}; CHECK(!ioctl(j, JSIOCGAXMAP, axmap) && axmap[0] == ABS_X && axmap[1] == ABS_Y && axmap[2] == ABS_Z && axmap[3] == ABS_HAT0X, "JSIOCGAXMAP");
    static unsigned short btnmap[KEY_MAX - BTN_MISC + 1]; CHECK(!ioctl(j, JSIOCGBTNMAP, btnmap), "JSIOCGBTNMAP");
    for (int i = 0; i < 12; i++) CHECK(btnmap[i] == JS_ORDER[i], "button %d = %x", i, btnmap[i]);
    /* events */
    double before = now_s(CLOCK_REALTIME);
    emit(u, EV_KEY, BTN_SOUTH, 1); emit(u, EV_ABS, ABS_X, 1000); emit(u, EV_ABS, ABS_HAT0X, 1); emit(u, EV_SYN, SYN_REPORT, 0);
    struct input_event got[64]; int n = read_events(e1, got, 64);
    CHECK(n == 4 && got[0].type == EV_KEY && got[0].code == BTN_SOUTH && got[0].value == 1 && got[1].type == EV_ABS && got[1].code == ABS_X && got[1].value == 1000 &&
          got[2].code == ABS_HAT0X && got[2].value == 1 && got[3].type == EV_SYN && got[3].code == SYN_REPORT, "first report: %d events", n);
    double stamp = n > 0 ? got[0].input_event_sec + got[0].input_event_usec / 1e6 : 0;
    CHECK(stamp >= before - 1 && stamp <= now_s(CLOCK_REALTIME) + 1, "CLOCK_REALTIME stamps (%.3f vs %.3f)", stamp, before);
    int clock = CLOCK_MONOTONIC; CHECK(!ioctl(e2, EVIOCSCLOCKID, &clock), "EVIOCSCLOCKID");
    n = read_events(e2, got, 64);                                         /* stamped when read: monotonic now */
    stamp = n > 0 ? got[0].input_event_sec + got[0].input_event_usec / 1e6 : 0;
    CHECK(n == 4 && stamp > 0 && absd(stamp - now_s(CLOCK_MONOTONIC)) < 2, "monotonic stamps %.3f", stamp);
    /* filtering: within fuzz, unchanged key, empty report */
    emit(u, EV_ABS, ABS_X, 1005); emit(u, EV_KEY, BTN_SOUTH, 1); emit(u, EV_SYN, SYN_REPORT, 0); emit(u, EV_SYN, SYN_REPORT, 0);
    errno = 0; CHECK(read_events(e1, got, 64) < 0 && errno == EAGAIN, "filtered events produce nothing");
    struct pollfd p = {e1, POLLIN, 0}; CHECK(poll(&p, 1, 0) == 0, "poll: nothing pending");
    unsigned char state[KEY_MAX / 8 + 1] = {0}; CHECK(ioctl(e1, EVIOCGKEY(sizeof(state)), state) >= 0 && test_bit(state, BTN_SOUTH), "EVIOCGKEY held");
    struct input_absinfo a; CHECK(!ioctl(e1, EVIOCGABS(ABS_X), &a) && a.value == 1000, "EVIOCGABS value %d", a.value);
    emit(u, EV_KEY, BTN_SOUTH, 0); emit(u, EV_SYN, SYN_REPORT, 0);
    CHECK(poll(&p, 1, 1000) == 1 && (p.revents & POLLIN), "poll: report pending");
    n = read_events(e1, got, 64); CHECK(n == 2 && got[0].code == BTN_SOUTH && got[0].value == 0, "release");
    read_events(e2, got, 64);
    /* joydev: initial state, then changes, correction */
    struct js_event je[64]; n = read_js(j, je, 64);
    CHECK(n == 16, "js init events %d", n);
    int init_buttons = 0, init_axes = 0;
    for (int i = 0; i < n; i++) {
        if (je[i].type == (JS_EVENT_INIT | JS_EVENT_BUTTON)) init_buttons++;
        if (je[i].type == (JS_EVENT_INIT | JS_EVENT_AXIS)) {
            init_axes++;
            if (je[i].number == 0) CHECK(je[i].value == js_expected(1000, -32768, 32767, 128), "js init axis 0 = %d", je[i].value);
            if (je[i].number == 3) CHECK(je[i].value == 32767, "js init hat = %d", je[i].value);
        }
    }
    CHECK(init_buttons == 12 && init_axes == 4, "init buttons=%d axes=%d", init_buttons, init_axes);
    errno = 0; CHECK(read_js(j, je, 64) < 0 && errno == EAGAIN, "js: nothing after the init state (BTN_SOUTH press/release preceded the open state)");
    emit(u, EV_KEY, BTN_EAST, 1); emit(u, EV_ABS, ABS_Y, -32768); emit(u, EV_KEY, BTN_DPAD_UP, 1); emit(u, EV_SYN, SYN_REPORT, 0);
    n = read_js(j, je, 64);
    CHECK(n == 3 && je[0].type == JS_EVENT_BUTTON && je[0].number == 1 && je[0].value == 1 && je[1].type == JS_EVENT_AXIS && je[1].number == 1 && je[1].value == -32767 &&
          je[2].type == JS_EVENT_BUTTON && je[2].number == 11 && je[2].value == 1, "js events: %d", n);
    errno = 0; CHECK(read_js(j, je, 64) < 0 && errno == EAGAIN, "js drained");
    errno = 0; CHECK(read(j, je, 4) < 0 && errno == EINVAL, "js short read");
    errno = 0; CHECK(write(j, je, sizeof(je[0])) < 0 && (errno == EINVAL || errno == EBADF), "js write");
    struct js_corr corr[ABS_CNT]; memset(corr, 0, sizeof(corr));
    CHECK(!ioctl(j, JSIOCGCORR, corr) && corr[0].type == JS_CORR_BROKEN && corr[0].coef[0] == -128 && corr[0].coef[1] == 128 && corr[0].coef[2] == (1 << 29) / 32511, "JSIOCGCORR");
    struct js_corr none[ABS_CNT]; memset(none, 0, sizeof(none));
    CHECK(!ioctl(j, JSIOCSCORR, none), "JSIOCSCORR none");
    emit(u, EV_ABS, ABS_X, 2000); emit(u, EV_SYN, SYN_REPORT, 0);
    n = read_js(j, je, 64); CHECK(n == 1 && je[0].number == 0 && je[0].value == 2000, "uncorrected axis: %d", n > 0 ? je[0].value : -1);
    none[2].type = 7; errno = 0; CHECK(ioctl(j, JSIOCSCORR, none) < 0 && errno == EINVAL, "JSIOCSCORR bad type");
    unsigned char badmap[ABS_CNT]; memcpy(badmap, axmap, sizeof(badmap)); badmap[1] = ABS_RX; errno = 0; CHECK(ioctl(j, JSIOCSAXMAP, badmap) < 0 && errno == EINVAL, "JSIOCSAXMAP to a missing axis");
    unsigned char swapped[ABS_CNT]; memcpy(swapped, axmap, sizeof(swapped)); swapped[0] = ABS_Y; swapped[1] = ABS_X;
    CHECK(!ioctl(j, JSIOCSAXMAP, swapped), "JSIOCSAXMAP swap");
    emit(u, EV_ABS, ABS_X, 3000); emit(u, EV_SYN, SYN_REPORT, 0);
    n = read_js(j, je, 64); CHECK(n == 1 && je[0].number == 1 && je[0].value == 3000, "swapped axis number %d", n > 0 ? je[0].number : -1);
    static unsigned short badbtn[KEY_MAX - BTN_MISC + 1]; memcpy(badbtn, btnmap, sizeof(badbtn)); badbtn[0] = KEY_A;
    errno = 0; CHECK(ioctl(j, JSIOCSBTNMAP, badbtn) < 0 && errno == EINVAL, "JSIOCSBTNMAP to a non-button");
    read_events(e1, got, 64); read_events(e2, got, 64);
    /* overflow: 1100 events without reading */
    for (int i = 0; i < 550; i++) { emit(u, EV_ABS, ABS_Z, i % 2 ? 200 : 100); emit(u, EV_SYN, SYN_REPORT, 0); }
    n = read_events(e2, got, 64);
    CHECK(n >= 1 && got[0].type == EV_SYN && got[0].code == SYN_DROPPED, "SYN_DROPPED after an overflow (%d events, first %d/%d)", n, n > 0 ? got[0].type : -1, n > 0 ? got[0].code : -1);
    errno = 0; CHECK(read_events(e2, got, 64) < 0 && errno == EAGAIN, "resynchronised after SYN_DROPPED");
    CHECK(!ioctl(e2, EVIOCGABS(ABS_Z), &a) && a.value == 200, "state after the overflow %d", a.value);
    n = read_js(j, je, 64); CHECK(n == 16 && (je[0].type & JS_EVENT_INIT), "joydev resends the state after an overflow (%d)", n);
    read_events(e1, got, 64); read_events(e1, got, 64);
    /* grab */
    CHECK(!ioctl(e1, EVIOCGRAB, 1), "EVIOCGRAB");
    errno = 0; CHECK(ioctl(e2, EVIOCGRAB, 1) < 0 && errno == EBUSY, "second grab");
    emit(u, EV_KEY, BTN_WEST, 1); emit(u, EV_SYN, SYN_REPORT, 0);
    CHECK(read_events(e1, got, 64) == 2, "grabber gets the report");
    errno = 0; CHECK(read_events(e2, got, 64) < 0 && errno == EAGAIN, "others see nothing while grabbed");
    errno = 0; CHECK(ioctl(e2, EVIOCGRAB, 0) < 0 && errno == EINVAL, "release by a non-grabber");
    CHECK(!ioctl(e1, EVIOCGRAB, 0), "release");
    emit(u, EV_KEY, BTN_WEST, 0); emit(u, EV_SYN, SYN_REPORT, 0);
    CHECK(read_events(e2, got, 64) == 2, "delivery after the release");
    read_events(e1, got, 64); read_js(j, je, 64);
    /* force feedback through the owner */
    check_force_feedback(u, event_path);
    check_permissions(event_path);
    fuzz(e1, j, u);
    /* revoke */
    int r = open(event_path, O_RDONLY | O_NONBLOCK);
    CHECK(r >= 0 && !ioctl(r, EVIOCREVOKE, 0), "EVIOCREVOKE");
    errno = 0; CHECK(r >= 0 && read_events(r, got, 4) < 0 && errno == ENODEV, "revoked fd reads ENODEV");
    if (r >= 0) close(r);
    /* destroy with fds open */
    CHECK(!ioctl(u, UI_DEV_DESTROY), "UI_DEV_DESTROY");
    errno = 0; CHECK(read_events(e1, got, 4) < 0 && errno == ENODEV, "event fd after unplug: ENODEV");
    errno = 0; CHECK(read_js(j, je, 4) < 0 && errno == ENODEV, "js fd after unplug: ENODEV");
    p = (struct pollfd){e1, POLLIN, 0}; CHECK(poll(&p, 1, 0) == 1 && (p.revents & (POLLERR | POLLHUP)), "poll after unplug: %x", p.revents);
    char text[64]; errno = 0; CHECK(ioctl(e2, EVIOCGNAME(sizeof(text)), text) < 0 && errno == ENODEV, "ioctl after unplug");
    errno = 0; CHECK(open(event_path, O_RDONLY) < 0 && errno == ENOENT, "node gone");
    errno = 0; CHECK(open(js_path, O_RDONLY) < 0 && errno == ENOENT, "js node gone");
    char sys[96]; snprintf(sys, sizeof(sys), "/sys/class/input/event%d/dev", event);
    CHECK(read_text(sys, text, sizeof(text)) < 0, "sysfs object gone");
    errno = 0; CHECK(ioctl(u, UI_DEV_DESTROY) < 0 && errno == EINVAL, "second destroy");
    close(e1); close(e2); close(j); close(u);
    CHECK(find_node("event", PAD_NAME, event_path, sizeof(event_path)) < 0 && count_nodes("js") == 0, "no pad left");
    check_console_merge();
    /* many devices: every uinput fd can create one; the event/js tables fill up cleanly */
    int fds[20], made = 0, refused = 0;
    for (int i = 0; i < 20; i++) {
        fds[i] = open("/dev/uinput", O_RDWR | O_NONBLOCK);
        if (fds[i] < 0) { refused++; continue; }
        struct uinput_setup s; memset(&s, 0, sizeof(s)); snprintf(s.name, sizeof(s.name), "Pad %d", i);
        ioctl(fds[i], UI_SET_EVBIT, EV_KEY); ioctl(fds[i], UI_SET_KEYBIT, BTN_SOUTH);
        if (!ioctl(fds[i], UI_DEV_SETUP, &s) && !ioctl(fds[i], UI_DEV_CREATE)) made++; else refused++;
    }
    CHECK(made >= 10 && made + refused == 20, "many devices: %d made, %d refused", made, refused);
    for (int i = 0; i < 20; i++) if (fds[i] >= 0) close(fds[i]);
    CHECK(count_nodes("js") == 0, "all gone");
    printf("[64] GAMEPAD uinput: %d devices at once\n", made);
}

/* ---------- the virtual Xbox 360 pad (vpad) ---------- */
static void campaign_vpad(void) {
    char event_path[64], js_path[64];
    int event = find_node("event", "Microsoft X-Box 360 pad", event_path, sizeof(event_path)), js = find_node("js", "Microsoft X-Box 360 pad", js_path, sizeof(js_path));
    CHECK(event >= 2 && js >= 0, "vpad nodes event%d js%d", event, js);
    if (event < 0 || js < 0) return;
    int fd = open(event_path, O_RDWR | O_NONBLOCK), j = open(js_path, O_RDONLY | O_NONBLOCK);
    CHECK(fd >= 0 && j >= 0, "open vpad");
    if (fd < 0 || j < 0) return;
    struct input_id id; CHECK(!ioctl(fd, EVIOCGID, &id) && id.bustype == BUS_USB && id.vendor == 0x045E && id.product == 0x028E, "vpad id");
    unsigned char axes = 0, buttons = 0; CHECK(!ioctl(j, JSIOCGAXES, &axes) && axes == 8 && !ioctl(j, JSIOCGBUTTONS, &buttons) && buttons == 11, "vpad js axes %d buttons %d", axes, buttons);
    unsigned char ffb[FF_MAX / 8 + 1] = {0}; CHECK(ioctl(fd, EVIOCGBIT(EV_FF, sizeof(ffb)), ffb) >= 0 && test_bit(ffb, FF_RUMBLE), "vpad rumble bit");
    /* 5 s of the pattern */
    bool south = false, east = false, right = false, left = false, down = false, trigger = false, hat = false; int js_events = 0;
    double deadline = now_s(CLOCK_MONOTONIC) + 5;
    while (now_s(CLOCK_MONOTONIC) < deadline) {
        struct pollfd p[2] = {{fd, POLLIN, 0}, {j, POLLIN, 0}}; poll(p, 2, 200);
        struct input_event got[64]; int n = read_events(fd, got, 64);
        for (int i = 0; i < n; i++) {
            if (got[i].type == EV_KEY && got[i].code == BTN_SOUTH && got[i].value == 1) south = true;
            if (got[i].type == EV_KEY && got[i].code == BTN_EAST && got[i].value == 1) east = true;
            if (got[i].type == EV_ABS && got[i].code == ABS_X && got[i].value == 32767) right = true;
            if (got[i].type == EV_ABS && got[i].code == ABS_X && got[i].value == -32768) left = true;
            if (got[i].type == EV_ABS && got[i].code == ABS_Y && got[i].value == 32767) down = true;
            if (got[i].type == EV_ABS && got[i].code == ABS_Z && got[i].value == 255) trigger = true;
            if (got[i].type == EV_ABS && got[i].code == ABS_HAT0Y && got[i].value == -1) hat = true;
        }
        struct js_event je[64]; int m = read_js(j, je, 64); if (m > 0) js_events += m;
    }
    CHECK(south && east && right && left && down && trigger && hat, "pattern: A=%d B=%d right=%d left=%d down=%d LT=%d up=%d", south, east, right, left, down, trigger, hat);
    CHECK(js_events >= 19 + 8, "joydev events %d", js_events);
    /* rumble: upload, play (the kernel logs the motor levels), stop after the replay length */
    struct ff_effect fx; memset(&fx, 0, sizeof(fx)); fx.type = FF_RUMBLE; fx.id = -1; fx.u.rumble.strong_magnitude = 0xC000; fx.u.rumble.weak_magnitude = 0x3000; fx.replay.length = 300;
    CHECK(!ioctl(fd, EVIOCSFF, &fx) && fx.id >= 0, "vpad upload");
    CHECK(!emit(fd, EV_FF, (unsigned)fx.id, 1), "vpad play");
    sleep_ms(600);
    CHECK(!ioctl(fd, EVIOCRMFF, fx.id), "vpad erase");
    printf("[64] GAMEPAD vpad: pattern seen, %d js events, rumble played\n", js_events);
    close(fd); close(j);
}

/* ---------- libudev (Debian's libudev.so.1, staged by the test as /nexus/udev/): Chrome's and SDL's enumeration ---------- */
struct udev; struct udev_enumerate; struct udev_list_entry; struct udev_device; struct udev_monitor;
static struct {
    struct udev* (*new_)(void); struct udev_enumerate* (*enumerate_new)(struct udev*); int (*add_match_subsystem)(struct udev_enumerate*, const char*);
    int (*scan)(struct udev_enumerate*); struct udev_list_entry* (*list)(struct udev_enumerate*); struct udev_list_entry* (*next)(struct udev_list_entry*);
    const char* (*name)(struct udev_list_entry*); struct udev_device* (*from_syspath)(struct udev*, const char*);
    const char* (*devnode)(struct udev_device*); const char* (*sysname)(struct udev_device*); const char* (*subsystem)(struct udev_device*);
    const char* (*property)(struct udev_device*, const char*); const char* (*sysattr)(struct udev_device*, const char*);
    struct udev_device* (*parent_with)(struct udev_device*, const char*, const char*); const char* (*syspath)(struct udev_device*);
    struct udev_device* (*unref)(struct udev_device*);
    struct udev_monitor* (*monitor_new)(struct udev*, const char*); int (*monitor_filter)(struct udev_monitor*, const char*, const char*);
    int (*monitor_enable)(struct udev_monitor*); int (*monitor_fd)(struct udev_monitor*); struct udev_device* (*monitor_receive)(struct udev_monitor*);
    const char* (*action)(struct udev_device*);
} U;
#include <dlfcn.h>
static bool load_udev(void) {
    if (!dlopen("/nexus/udev/libcap.so.2", RTLD_NOW | RTLD_GLOBAL)) { printf("[64] GAMEPAD udev: %s\n", dlerror()); return false; }
    void* h = dlopen("/nexus/udev/libudev.so.1", RTLD_NOW);
    if (!h) { printf("[64] GAMEPAD udev: %s\n", dlerror()); return false; }
    #define SYM(field, name) do { *(void**)&U.field = dlsym(h, name); if (!U.field) { printf("[64] GAMEPAD udev: no %s\n", name); return false; } } while (0)
    SYM(new_, "udev_new"); SYM(enumerate_new, "udev_enumerate_new"); SYM(add_match_subsystem, "udev_enumerate_add_match_subsystem");
    SYM(scan, "udev_enumerate_scan_devices"); SYM(list, "udev_enumerate_get_list_entry"); SYM(next, "udev_list_entry_get_next");
    SYM(name, "udev_list_entry_get_name"); SYM(from_syspath, "udev_device_new_from_syspath"); SYM(devnode, "udev_device_get_devnode");
    SYM(sysname, "udev_device_get_sysname"); SYM(subsystem, "udev_device_get_subsystem"); SYM(property, "udev_device_get_property_value");
    SYM(sysattr, "udev_device_get_sysattr_value"); SYM(parent_with, "udev_device_get_parent_with_subsystem_devtype"); SYM(syspath, "udev_device_get_syspath");
    SYM(unref, "udev_device_unref"); SYM(monitor_new, "udev_monitor_new_from_netlink"); SYM(monitor_filter, "udev_monitor_filter_add_match_subsystem_devtype");
    SYM(monitor_enable, "udev_monitor_enable_receiving"); SYM(monitor_fd, "udev_monitor_get_fd"); SYM(monitor_receive, "udev_monitor_receive_device");
    SYM(action, "udev_device_get_action");
    #undef SYM
    return true;
}
static const char* safe(const char* text) { return text ? text : "(null)"; }
/* GamepadPlatformDataFetcherLinux: every "input" device with ID_INPUT_JOYSTICK=1, node jsN/eventN, parent input device's
 * name and id/{vendor,product,version} sysattrs. Returns how many joystick nodes were found ("js" and "event"). */
static int udev_scan(struct udev* u, const char* want, bool verbose, int* js_out, int* event_out) {
    struct udev_enumerate* en = U.enumerate_new(u); int found = 0; *js_out = *event_out = 0;
    int matched = en ? U.add_match_subsystem(en, "input") : -1, scanned = en ? U.scan(en) : -1;
    CHECK(en && matched >= 0 && scanned >= 0, "udev enumerate: %p match %d scan %d", (void*)en, matched, scanned);
    if (!en) return 0;
    for (struct udev_list_entry* e = U.list(en); e; e = U.next(e)) {
        struct udev_device* dev = U.from_syspath(u, U.name(e));
        if (verbose) printf("[64] GAMEPAD udev: %s -> %s node=%s subsystem=%s joystick=%s\n", U.name(e), dev ? safe(U.sysname(dev)) : "(no device)",
                            dev ? safe(U.devnode(dev)) : "-", dev ? safe(U.subsystem(dev)) : "-", dev ? safe(U.property(dev, "ID_INPUT_JOYSTICK")) : "-");
        if (!dev) continue;
        const char* joystick = U.property(dev, "ID_INPUT_JOYSTICK"), *node = U.devnode(dev), *sysname = U.sysname(dev);
        if (joystick && !strcmp(joystick, "1") && node && sysname && (!strncmp(sysname, "js", 2) || !strncmp(sysname, "event", 5))) {
            struct udev_device* parent = U.parent_with(dev, "input", NULL);
            const char* name = parent ? U.sysattr(parent, "name") : NULL, *vendor = parent ? U.sysattr(parent, "id/vendor") : NULL;
            const char* product = parent ? U.sysattr(parent, "id/product") : NULL;
            if (verbose) printf("[64] GAMEPAD udev:   parent %s name='%s' vendor=%s product=%s\n", parent ? safe(U.syspath(parent)) : "(none)", safe(name), safe(vendor), safe(product));
            if (name && !strcmp(name, want) && vendor && product) {
                found++;
                if (!strncmp(sysname, "js", 2)) (*js_out)++; else (*event_out)++;
                int fd = open(node, O_RDONLY | O_NONBLOCK); CHECK(fd >= 0, "open %s", node);
                if (fd >= 0) close(fd);
            }
        }
        U.unref(dev);
    }
    return found;
}
/* systemd's chase() walk (O_PATH|O_NOFOLLOW openat, fstat, readlinkat on the parent, ".." with inode checks) */
static int chase_walk(const char* path, bool verbose) {
    int fd = open("/", O_CLOEXEC | O_DIRECTORY | O_PATH); if (fd < 0) { printf("[64] GAMEPAD chase: open / errno %d\n", errno); return -errno; }
    char todo[512]; snprintf(todo, sizeof(todo), "%s", path); int follow = 32; char* p = todo;
    while (*p) {
        while (*p == '/') p++;
        if (!*p) break;
        char first[256]; unsigned n = 0; while (p[n] && p[n] != '/' && n < 255) { first[n] = p[n]; n++; } first[n] = 0; p += n;
        if (!strcmp(first, ".")) continue;
        if (!strcmp(first, "..")) {
            int parent = openat(fd, "..", O_CLOEXEC | O_NOFOLLOW | O_PATH | O_DIRECTORY);
            struct stat a, b; if (parent < 0 || fstat(parent, &a) || fstat(fd, &b)) { printf("[64] GAMEPAD chase: .. errno %d\n", errno); return -errno; }
            close(fd); fd = parent; continue;
        }
        int child = openat(fd, first, O_CLOEXEC | O_NOFOLLOW | O_PATH);
        if (child < 0) { printf("[64] GAMEPAD chase: openat(%s) errno %d\n", first, errno); close(fd); return -errno; }
        struct stat st; if (fstat(child, &st)) { printf("[64] GAMEPAD chase: fstat(%s) errno %d\n", first, errno); return -errno; }
        if (verbose) printf("[64] GAMEPAD chase: %s mode %o ino %llu\n", first, st.st_mode, (unsigned long long)st.st_ino);
        if (S_ISLNK(st.st_mode)) {
            char target[512]; ssize_t length = readlinkat(fd, first, target, sizeof(target) - 1);
            if (length <= 0) { printf("[64] GAMEPAD chase: readlinkat(%s) = %zd errno %d\n", first, length, errno); return -22; }
            target[length] = 0; if (verbose) printf("[64] GAMEPAD chase:   -> %s\n", target);
            if (--follow <= 0) return -40;
            char rest[512]; snprintf(rest, sizeof(rest), "%s%s", target, p); snprintf(todo, sizeof(todo), "%s", rest); p = todo;
            if (target[0] == '/') { close(fd); fd = open("/", O_CLOEXEC | O_DIRECTORY | O_PATH); }
            close(child); continue;
        }
        close(fd); fd = child;
        if (*p && !S_ISDIR(st.st_mode)) { printf("[64] GAMEPAD chase: %s not a directory\n", first); return -20; }
    }
    struct stat st; int r = fstat(fd, &st); close(fd); return r ? -errno : 0;
}
static void list_dir(const char* path) {
    DIR* d = opendir(path); if (!d) { printf("[64] GAMEPAD readdir %s: errno %d\n", path, errno); return; }
    for (struct dirent* e = readdir(d); e; e = readdir(d)) printf("[64] GAMEPAD readdir %s: %s type %d\n", path, e->d_name, e->d_type);
    closedir(d);
}
static void campaign_udev(void) {
    if (access("/nexus/udev/debug", F_OK) == 0) {
        const char* walks[] = {"/sys/class/input/js0", "/sys/bus/input/devices/input0", "/sys/class/input/input0", "/sys/devices/virtual/input/input0/js0/subsystem"};
        for (unsigned i = 0; i < 4; i++) { int r = chase_walk(walks[i], true); printf("[64] GAMEPAD chase %s = %d\n", walks[i], r); }
        list_dir("/sys/class/input"); list_dir("/sys/bus"); list_dir("/sys/bus/input/devices"); list_dir("/sys/devices/virtual/input/input0");
    }
    if (getenv("NEXUS_UDEV_DEBUG") || access("/nexus/udev/debug", F_OK) == 0) { setenv("SYSTEMD_LOG_LEVEL", "debug", 1); setenv("SYSTEMD_LOG_TARGET", "console", 1); fflush(stdout); dup2(1, 2); }
    if (!load_udev()) { CHECK(0, "libudev not loadable"); return; }
    struct udev* u = U.new_(); CHECK(u != NULL, "udev_new"); if (!u) return;
    if (access("/nexus/udev/debug", F_OK) == 0) {
        const char* paths[] = {"/sys/class/input/js0", "/sys/devices/virtual/input/input0", "/sys/devices/virtual/input/input0/js0", "/sys/bus/input/devices/input0"};
        for (unsigned i = 0; i < 4; i++) {
            errno = 0; struct udev_device* dev = U.from_syspath(u, paths[i]);
            printf("[64] GAMEPAD udev_device_new_from_syspath(%s) = %p errno %d", paths[i], (void*)dev, errno);
            if (dev) printf(" syspath=%s subsystem=%s devnode=%s joystick=%s", safe(U.syspath(dev)), safe(U.subsystem(dev)), safe(U.devnode(dev)), safe(U.property(dev, "ID_INPUT_JOYSTICK")));
            printf("\n");
            if (dev) U.unref(dev);
        }
    }
    int js = 0, event = 0;
    int found = udev_scan(u, "Microsoft X-Box 360 pad", true, &js, &event);
    CHECK(found == 2 && js == 1 && event == 1, "libudev finds the vpad as js + event joystick nodes: %d (js %d event %d)", found, js, event);
    /* hot-plug through the udev monitor (kernel uevents on NETLINK_KOBJECT_UEVENT, no udevd): a uinput pad comes and goes */
    struct udev_monitor* m = U.monitor_new(u, "kernel");
    CHECK(m && U.monitor_filter(m, "input", NULL) >= 0 && U.monitor_enable(m) >= 0, "udev monitor");
    int uin = create_pad(); int adds = 0, removes = 0;
    if (uin >= 0) {
        found = udev_scan(u, PAD_NAME, false, &js, &event);
        CHECK(found == 2, "libudev finds a uinput pad: %d", found);
        ioctl(uin, UI_DEV_DESTROY); close(uin);
    }
    for (int i = 0; m && i < 40; i++) {
        struct pollfd p = {U.monitor_fd(m), POLLIN, 0}; if (poll(&p, 1, 50) <= 0) continue;
        struct udev_device* dev = U.monitor_receive(m); if (!dev) continue;
        const char* action = U.action(dev), *sysname = U.sysname(dev);
        if (sysname && !strncmp(sysname, "js", 2)) { if (action && !strcmp(action, "add")) adds++; if (action && !strcmp(action, "remove")) removes++; }
        U.unref(dev);
    }
    CHECK(adds == 1 && removes == 1, "udev monitor: js add %d remove %d", adds, removes);
    printf("[64] GAMEPAD udev: vpad found through libudev (js + event), uinput hot-plug seen by the monitor\n");
}

int main(void) {
    setvbuf(stdout, NULL, _IOLBF, 0);
    char mode = 0; FILE* config = fopen("/nexus/test-hw", "r");
    if (config) { mode = (char)fgetc(config); fclose(config); }
    printf("[64] GAMEPAD TEST START mode=%c\n", mode);
    if (mode == 'u') campaign_uinput();
    else if (mode == 'p') campaign_vpad();
    else if (mode == 'd') campaign_udev();
    else if (mode == 'n') {
        CHECK(count_nodes("js") == 0, "no joystick without vpad");
        struct stat st; CHECK(!stat("/dev/uinput", &st) && S_ISCHR(st.st_mode) && major(st.st_rdev) == 10 && minor(st.st_rdev) == 223, "/dev/uinput 10:223");
        CHECK(count_nodes("event") == 2, "only the console event nodes (%d)", count_nodes("event"));
    } else CHECK(0, "unknown mode %c", mode);
    printf(failures ? "[64] GAMEPAD TEST FAILED failures=%d\n" : "[64] GAMEPAD TEST PASSED failures=%d\n", failures);
    return failures ? 1 : 0;
}
