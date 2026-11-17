/* USB Audio Class test program (tests/test_x64_usbaudio.py), linked against the unmodified Debian libasound.so.2 and run
   by the kernel's /nexus/test-audio campaign. Mode = first byte of /nexus/test-audio:
   'u' USB card only (it becomes card 0): enumeration, controls (PCM Playback Volume/Switch, dB TLV), 440 Hz 2 s via
       "default" (blocking writei, consumption-rate fit), 660 Hz 1 s via hw:0,0 mmap, -12 dB 880 Hz, muted 550 Hz,
       330 Hz 1 s at 44.1 kHz through the plug, then hostile requests (unsupported formats/rates/channels).
   'b' HDA (card 0) + USB (card 1): node layout, both cards play at once (880 Hz on HDA, 440 Hz on USB), card-name opens.
   'h' hot-plug: waits for the card the host adds, plays until the host unplugs it mid-stream (the stream must fail
       with ENODEV, not hang), waits until the card is gone, plays again on the re-plugged card, sees the second unplug;
       uevents (SUBSYSTEM=sound) checked on a NETLINK_KOBJECT_UEVENT socket.
   Host markers: "[64] UAUDIO READY", "[64] UAUDIO PLAYING", "[64] UAUDIO WAIT-REPLUG", "[64] UAUDIO WAIT-UNPLUG". */
#define _GNU_SOURCE
#include <errno.h>
#include <fcntl.h>
#include <math.h>
#include <poll.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>
#include <sys/ioctl.h>
#include <sys/socket.h>
#include <sys/wait.h>
#include <sys/stat.h>
#include <sys/sysmacros.h>
#include <linux/netlink.h>
#include "aurora_sound.h"

typedef struct _snd_pcm snd_pcm_t;
typedef long snd_pcm_sframes_t;
typedef unsigned long snd_pcm_uframes_t;
typedef struct { void* addr; unsigned int first, step; } snd_pcm_channel_area_t;
typedef struct _snd_mixer snd_mixer_t;
typedef struct _snd_mixer_elem snd_mixer_elem_t;
typedef struct _snd_pcm_hw_params snd_pcm_hw_params_t;
int snd_pcm_open(snd_pcm_t** pcm, const char* name, int stream, int mode);
int snd_pcm_set_params(snd_pcm_t* pcm, int format, int access, unsigned channels, unsigned rate, int soft_resample, unsigned latency);
int snd_pcm_get_params(snd_pcm_t* pcm, snd_pcm_uframes_t* buffer_size, snd_pcm_uframes_t* period_size);
snd_pcm_sframes_t snd_pcm_writei(snd_pcm_t* pcm, const void* buffer, snd_pcm_uframes_t size);
int snd_pcm_delay(snd_pcm_t* pcm, snd_pcm_sframes_t* delay);
snd_pcm_sframes_t snd_pcm_avail_update(snd_pcm_t* pcm);
int snd_pcm_state(snd_pcm_t* pcm);
int snd_pcm_recover(snd_pcm_t* pcm, int error, int silent);
int snd_pcm_drain(snd_pcm_t* pcm);
int snd_pcm_drop(snd_pcm_t* pcm);
int snd_pcm_close(snd_pcm_t* pcm);
int snd_pcm_start(snd_pcm_t* pcm);
int snd_pcm_wait(snd_pcm_t* pcm, int timeout);
int snd_pcm_nonblock(snd_pcm_t* pcm, int nonblock);
int snd_pcm_mmap_begin(snd_pcm_t* pcm, const snd_pcm_channel_area_t** areas, snd_pcm_uframes_t* offset, snd_pcm_uframes_t* frames);
snd_pcm_sframes_t snd_pcm_mmap_commit(snd_pcm_t* pcm, snd_pcm_uframes_t offset, snd_pcm_uframes_t frames);
int snd_pcm_hw_params_malloc(snd_pcm_hw_params_t** params);
void snd_pcm_hw_params_free(snd_pcm_hw_params_t* params);
int snd_pcm_hw_params_any(snd_pcm_t* pcm, snd_pcm_hw_params_t* params);
int snd_pcm_hw_params_test_rate(snd_pcm_t* pcm, snd_pcm_hw_params_t* params, unsigned rate, int dir);
int snd_pcm_hw_params_test_channels(snd_pcm_t* pcm, snd_pcm_hw_params_t* params, unsigned channels);
int snd_pcm_hw_params_test_format(snd_pcm_t* pcm, snd_pcm_hw_params_t* params, int format);
const char* snd_strerror(int error);
int snd_card_next(int* card);
int snd_card_get_name(int card, char** name);
int snd_card_get_longname(int card, char** name);
int snd_mixer_open(snd_mixer_t** mixer, int mode);
int snd_mixer_attach(snd_mixer_t* mixer, const char* name);
int snd_mixer_selem_register(snd_mixer_t* mixer, void* options, void* classp);
int snd_mixer_load(snd_mixer_t* mixer);
int snd_mixer_close(snd_mixer_t* mixer);
snd_mixer_elem_t* snd_mixer_first_elem(snd_mixer_t* mixer);
snd_mixer_elem_t* snd_mixer_elem_next(snd_mixer_elem_t* elem);
const char* snd_mixer_selem_get_name(snd_mixer_elem_t* elem);
int snd_mixer_selem_has_playback_volume(snd_mixer_elem_t* elem);
int snd_mixer_selem_has_playback_switch(snd_mixer_elem_t* elem);
int snd_mixer_selem_get_playback_volume_range(snd_mixer_elem_t* elem, long* min, long* max);
int snd_mixer_selem_get_playback_dB_range(snd_mixer_elem_t* elem, long* min, long* max);
int snd_mixer_selem_set_playback_dB_all(snd_mixer_elem_t* elem, long value, int dir);
int snd_mixer_selem_get_playback_dB(snd_mixer_elem_t* elem, int channel, long* value);
int snd_mixer_selem_set_playback_switch_all(snd_mixer_elem_t* elem, int value);
int snd_mixer_selem_get_playback_switch(snd_mixer_elem_t* elem, int channel, int* value);
enum { PLAYBACK = 0, NONBLOCK = 1, S16_LE = 2, S24_3LE = 32, MMAP_INTERLEAVED = 0, RW_INTERLEAVED = 3, STATE_RUNNING = 3, STATE_DISCONNECTED = 8 };

static int failures;
#define CHECK(test, ...) do { if (!(test)) { printf("[64] UAUDIO FAIL line=%d: ", __LINE__); printf(__VA_ARGS__); printf("\n"); fflush(stdout); failures++; } } while (0)
static double now_s(void) { struct timespec t; clock_gettime(CLOCK_MONOTONIC, &t); return t.tv_sec + t.tv_nsec / 1e9; }
static void sleep_ms(double ms) { struct timespec t = {(time_t)(ms / 1000), (long)(fmod(ms, 1000) * 1e6)}; nanosleep(&t, NULL); }
static void tone(int16_t* out, unsigned frames, double* phase, double frequency, unsigned rate, double amplitude) {
    for (unsigned i = 0; i < frames; i++) {
        int16_t v = (int16_t)lrint(amplitude * 32767.0 * sin(*phase)); out[2 * i] = out[2 * i + 1] = v;
        *phase += 2 * M_PI * frequency / rate; if (*phase > 2 * M_PI) *phase -= 2 * M_PI;
    }
}
static snd_pcm_t* open_pcm(const char* name, int mode, int access, unsigned rate, unsigned latency) {
    snd_pcm_t* pcm = NULL; int error = snd_pcm_open(&pcm, name, PLAYBACK, mode);
    CHECK(error == 0, "snd_pcm_open(%s): %s", name, snd_strerror(error));
    if (error) return NULL;
    error = snd_pcm_set_params(pcm, S16_LE, access, 2, rate, 1, latency);
    CHECK(error == 0, "snd_pcm_set_params(%s, %u Hz): %s", name, rate, snd_strerror(error));
    if (error) { snd_pcm_close(pcm); return NULL; }
    snd_pcm_uframes_t buffer = 0, period = 0; snd_pcm_get_params(pcm, &buffer, &period);
    printf("[64] UAUDIO open %s rate=%u buffer=%lu period=%lu\n", name, rate, buffer, period); fflush(stdout);
    return pcm;
}
struct fit { double n, t, f, tt, tf; };
static void fit_add(struct fit* s, double t, double f) { s->n++; s->t += t; s->f += f; s->tt += t * t; s->tf += t * f; }
static double fit_slope(const struct fit* s) { return (s->n * s->tf - s->t * s->f) / (s->n * s->tt - s->t * s->t); }

/* Blocking writei of `seconds` of a tone; returns the error that ended it early (0 = all written). */
static int play_blocking(const char* name, unsigned rate, double frequency, double seconds, double amplitude, double* fitted, bool marker) {
    snd_pcm_t* pcm = open_pcm(name, 0, RW_INTERLEAVED, rate, 100000); if (!pcm) return -1;
    int16_t chunk[480 * 2]; unsigned step = rate / 100; double phase = 0, start = now_s(), next = start;
    long written = 0, total = (long)(rate * seconds); struct fit f = {0}; int result = 0, xruns = 0;
    while (written < total) {
        tone(chunk, step, &phase, frequency, rate, amplitude);
        snd_pcm_sframes_t n = snd_pcm_writei(pcm, chunk, step);
        if (n == -EPIPE) { xruns++; snd_pcm_recover(pcm, (int)n, 1); continue; }
        if (n < 0) { result = (int)n; break; }
        written += n;
        if (marker && written >= (long)rate / 2 && written - n < (long)rate / 2) { printf("[64] UAUDIO PLAYING %s\n", name); fflush(stdout); }
        double t = now_s(); snd_pcm_sframes_t delay = 0;
        if (t >= next && snd_pcm_state(pcm) == STATE_RUNNING && snd_pcm_delay(pcm, &delay) == 0) { next = t + 0.05; fit_add(&f, t - start, (double)(written - delay)); }
    }
    if (!result) { int error = snd_pcm_drain(pcm); CHECK(error == 0, "drain %s: %s", name, snd_strerror(error)); }
    double slope = f.n > 4 ? fit_slope(&f) : 0; if (fitted) *fitted = slope;
    printf("[64] UAUDIO played %s %.0f Hz frames=%ld xruns=%d rate_fit=%.1f result=%d elapsed=%.3f\n", name, frequency, written, xruns, slope, result, now_s() - start);
    fflush(stdout);
    snd_pcm_close(pcm);
    if (!result) CHECK(xruns == 0, "%s: %d underruns", name, xruns);
    return result;
}
static void play_mmap(const char* name, double frequency, double seconds) {
    snd_pcm_t* pcm = open_pcm(name, NONBLOCK, MMAP_INTERLEAVED, 48000, 50000); if (!pcm) return;
    double phase = 0; long written = 0, total = (long)(48000 * seconds); bool started = false; int waits = 0;
    while (written < total) {
        snd_pcm_sframes_t space = snd_pcm_avail_update(pcm);
        if (space < 0) { CHECK(0, "mmap avail: %s", snd_strerror((int)space)); break; }
        if (space < 480) { if (!started) { snd_pcm_start(pcm); started = true; } snd_pcm_wait(pcm, 1000); waits++; continue; }
        const snd_pcm_channel_area_t* areas; snd_pcm_uframes_t offset, frames = (snd_pcm_uframes_t)space;
        if (frames > (snd_pcm_uframes_t)(total - written)) frames = (snd_pcm_uframes_t)(total - written);
        int error = snd_pcm_mmap_begin(pcm, &areas, &offset, &frames);
        CHECK(error == 0, "mmap_begin: %s", snd_strerror(error)); if (error) break;
        int16_t* base = (int16_t*)areas[0].addr + offset * 2;
        tone(base, (unsigned)frames, &phase, frequency, 48000, 0.5);
        CHECK(snd_pcm_mmap_commit(pcm, offset, frames) == (snd_pcm_sframes_t)frames, "mmap_commit"); written += (long)frames;
    }
    if (!started) snd_pcm_start(pcm);
    snd_pcm_nonblock(pcm, 0);   /* a non-blocking drain returns -EAGAIN */
    CHECK(snd_pcm_drain(pcm) == 0, "mmap drain");
    printf("[64] UAUDIO mmap %s %.0f Hz frames=%ld waits=%d\n", name, frequency, written, waits); fflush(stdout);
    snd_pcm_close(pcm);
}
static snd_mixer_elem_t* find_element(snd_mixer_t* mixer, const char* wanted) {
    for (snd_mixer_elem_t* e = snd_mixer_first_elem(mixer); e; e = snd_mixer_elem_next(e)) if (!strcmp(snd_mixer_selem_get_name(e), wanted)) return e;
    return NULL;
}
static snd_mixer_t* mixer_open(const char* card) {
    snd_mixer_t* mixer = NULL; int error = snd_mixer_open(&mixer, 0);
    if (!error) error = snd_mixer_attach(mixer, card);
    if (!error) error = snd_mixer_selem_register(mixer, NULL, NULL);
    if (!error) error = snd_mixer_load(mixer);
    CHECK(error == 0, "mixer on %s: %s", card, snd_strerror(error));
    if (error && mixer) { snd_mixer_close(mixer); mixer = NULL; }
    return mixer;
}
static bool card_named(int card, const char* driver_name) {
    char* name = NULL; bool ok = !snd_card_get_name(card, &name) && name && strstr(name, driver_name); free(name); return ok;
}
static int card_count(void) { int card = -1, n = 0; while (!snd_card_next(&card) && card >= 0) n++; return n; }
static bool node(const char* path, unsigned minor) { struct stat st; return !stat(path, &st) && S_ISCHR(st.st_mode) && major(st.st_rdev) == 116 && minor(st.st_rdev) == minor; }

static void enumerate_usb(int card) {
    char* name = NULL; char* longname = NULL;
    CHECK(!snd_card_get_name(card, &name) && name, "card %d name", card);
    CHECK(!snd_card_get_longname(card, &longname) && longname && strstr(longname, "USB Audio 1.0, full speed"), "longname %s", longname ? longname : "?");
    printf("[64] UAUDIO card%d name=%s longname=%s\n", card, name ? name : "?", longname ? longname : "?"); free(name); free(longname);
    char path[64]; snprintf(path, sizeof(path), "/dev/snd/controlC%d", card); CHECK(node(path, card * 32), "%s", path);
    snprintf(path, sizeof(path), "/dev/snd/pcmC%dD0p", card); CHECK(node(path, card * 32 + 16), "%s", path);
    snprintf(path, sizeof(path), "/dev/snd/pcmC%dD0c", card); struct stat st; CHECK(stat(path, &st) == -1 && errno == ENOENT, "QEMU usb-audio has no capture: %s", path);
}
/* Unsupported requests must fail cleanly (the card stays usable). */
static void hostile(const char* hw) {
    snd_pcm_t* pcm = NULL; int error = snd_pcm_open(&pcm, hw, PLAYBACK, 0);
    CHECK(!error, "open %s", hw); if (error) return;
    snd_pcm_hw_params_t* p = NULL; snd_pcm_hw_params_malloc(&p); snd_pcm_hw_params_any(pcm, p);
    CHECK(snd_pcm_hw_params_test_rate(pcm, p, 44100, 0) != 0, "hw device must not offer 44.1 kHz");
    CHECK(snd_pcm_hw_params_test_channels(pcm, p, 6) != 0, "hw device must not offer 6 channels");
    CHECK(snd_pcm_hw_params_test_format(pcm, p, S24_3LE) != 0, "hw device must not offer S24_3LE");
    CHECK(snd_pcm_hw_params_test_rate(pcm, p, 48000, 0) == 0, "48 kHz offered");
    snd_pcm_hw_params_free(p);
    error = snd_pcm_set_params(pcm, S16_LE, RW_INTERLEAVED, 2, 96000, 0, 100000);
    CHECK(error < 0, "96 kHz without resampling must be refused");
    snd_pcm_close(pcm);
    /* a garbage ioctl on the control node */
    int fd = open("/dev/snd/controlC0", O_RDWR); CHECK(fd >= 0, "controlC0 open");
    if (fd >= 0) { CHECK(ioctl(fd, 0xC0FF5599, (void*)8) < 0, "garbage ioctl"); close(fd); }
}
static void mode_usb(void) {
    CHECK(card_count() == 1 && card_named(0, "Audio"), "one card: the USB device");
    enumerate_usb(0);
    snd_mixer_t* mixer = mixer_open("hw:0");
    snd_mixer_elem_t* pcm = mixer ? find_element(mixer, "PCM") : NULL;
    CHECK(pcm && snd_mixer_selem_has_playback_volume(pcm) && snd_mixer_selem_has_playback_switch(pcm), "PCM volume+switch element");
    long min = 0, max = 0, dmin = 0, dmax = 0;
    if (pcm) {
        snd_mixer_selem_get_playback_volume_range(pcm, &min, &max); snd_mixer_selem_get_playback_dB_range(pcm, &dmin, &dmax);
        printf("[64] UAUDIO PCM volume %ld..%ld dB %.2f..%.2f\n", min, max, dmin / 100.0, dmax / 100.0);
        CHECK(max > min && dmax > dmin && dmax <= 900 && dmin < -6000, "volume range/dB from the feature unit");
        CHECK(snd_mixer_selem_set_playback_dB_all(pcm, 0, 0) == 0, "0 dB");
        int on = 0; CHECK(snd_mixer_selem_set_playback_switch_all(pcm, 1) == 0 && !snd_mixer_selem_get_playback_switch(pcm, 0, &on) && on, "unmuted");
    }
    double fit = 0;
    CHECK(play_blocking("default", 48000, 440, 2.0, 0.5, &fit, false) == 0, "440 Hz on default");
    CHECK(fit > 48000 * 0.97 && fit < 48000 * 1.03, "USB consumption rate %.1f frames/s", fit);
    play_mmap("hw:0,0", 660, 1.0);
    if (pcm) {
        CHECK(snd_mixer_selem_set_playback_dB_all(pcm, -1200, 0) == 0, "-12 dB");
        long db = 0; snd_mixer_selem_get_playback_dB(pcm, 0, &db); printf("[64] UAUDIO volume now %.2f dB\n", db / 100.0);
        CHECK(db <= -1100 && db >= -1300, "volume read back %ld", db);
        sleep_ms(200);   /* the USB worker applies SET_CUR */
    }
    play_blocking("default", 48000, 880, 1.0, 0.5, NULL, false);
    if (pcm) { CHECK(snd_mixer_selem_set_playback_dB_all(pcm, 0, 0) == 0, "0 dB again"); CHECK(snd_mixer_selem_set_playback_switch_all(pcm, 0) == 0, "mute"); sleep_ms(200); }
    play_blocking("default", 48000, 550, 0.5, 0.5, NULL, false);
    if (pcm) { CHECK(snd_mixer_selem_set_playback_switch_all(pcm, 1) == 0, "unmute"); sleep_ms(200); }
    play_blocking("default", 44100, 330, 1.0, 0.5, NULL, false);
    if (mixer) snd_mixer_close(mixer);
    hostile("hw:0,0");
}
/* Two cards at once: the HDA tone (card 0, 880 Hz) and the USB tone (card 1, 440 Hz) play concurrently from two
   processes; the host checks each capture holds only its own tone. */
static void mode_both(void) {
    CHECK(card_count() == 2 && card_named(0, "HDA") && card_named(1, "Audio"), "card 0 HDA + card 1 USB");
    enumerate_usb(1);
    CHECK(node("/dev/snd/controlC0", 0) && node("/dev/snd/pcmC0D0p", 16), "card 0 nodes");
    pid_t child = fork();
    if (!child) { int r = play_blocking("hw:0,0", 48000, 880, 2.0, 0.5, NULL, false); _exit(r ? 1 : 0); }
    play_blocking("default:CARD=Audio", 48000, 440, 2.0, 0.5, NULL, false);
    int status = 0; waitpid(child, &status, 0); CHECK(WIFEXITED(status) && WEXITSTATUS(status) == 0, "HDA player");
    play_blocking("plughw:1,0", 44100, 660, 1.0, 0.5, NULL, false);
}
/* uevent socket: the "add"/"remove" lines for the sound nodes. */
static int uevents = -1;
static void uevent_open(void) {
    uevents = socket(AF_NETLINK, SOCK_DGRAM | SOCK_NONBLOCK | SOCK_CLOEXEC, NETLINK_KOBJECT_UEVENT);
    struct sockaddr_nl a = {.nl_family = AF_NETLINK, .nl_groups = 1};
    CHECK(uevents >= 0 && bind(uevents, (struct sockaddr*)&a, sizeof(a)) == 0, "uevent socket");
}
static int uevent_count(const char* action) {
    int count = 0; char buffer[2048];
    for (;;) {
        ssize_t n = recv(uevents, buffer, sizeof(buffer) - 1, 0); if (n <= 0) break; buffer[n] = 0;
        if (!strncmp(buffer, action, strlen(action)) && memmem(buffer, (size_t)n, "SUBSYSTEM=sound", 15)) { count++; printf("[64] UAUDIO uevent %s\n", buffer); }
    }
    return count;
}
static bool wait_cards(int wanted, double seconds) {
    double deadline = now_s() + seconds;
    while (now_s() < deadline) { if (card_count() == wanted) return true; sleep_ms(100); }
    return card_count() == wanted;
}
static void mode_hotplug(void) {
    uevent_open();
    CHECK(card_count() == 0, "no card before the plug");
    printf("[64] UAUDIO READY\n"); fflush(stdout);
    CHECK(wait_cards(1, 60), "USB card appeared");
    sleep_ms(300); CHECK(uevent_count("add@") >= 2, "add uevents (control + pcm)");
    enumerate_usb(0);
    /* Play 20 s; the host unplugs after "PLAYING": writei must fail with ENODEV within a bounded time. */
    double start = now_s();
    int result = play_blocking("hw:0,0", 48000, 440, 20.0, 0.5, NULL, true);
    printf("[64] UAUDIO unplug result=%d (%s) after %.3f s\n", result, result ? snd_strerror(result) : "none", now_s() - start);
    CHECK(result == -ENODEV, "stream ended with ENODEV, got %d", result);
    CHECK(wait_cards(0, 10), "card gone after unplug");
    sleep_ms(300); CHECK(uevent_count("remove@") >= 2, "remove uevents");
    struct stat st; CHECK(stat("/dev/snd/pcmC0D0p", &st) == -1 && errno == ENOENT, "nodes removed");
    printf("[64] UAUDIO WAIT-REPLUG\n"); fflush(stdout);
    CHECK(wait_cards(1, 60), "card back after re-plug");
    CHECK(play_blocking("default", 48000, 660, 1.0, 0.5, NULL, false) == 0, "660 Hz on the re-plugged card");
    printf("[64] UAUDIO WAIT-UNPLUG\n"); fflush(stdout);
    CHECK(wait_cards(0, 60), "card gone after the second unplug");
    close(uevents);
}
/* A device plugged while this program runs stays plugged when it exits: the kernel's per-program allocation audit
   (run_image) must not count the driver's device-lifetime memory (counted allocations, storage_alloc.c). */
static void mode_stay(void) {
    CHECK(card_count() == 0, "no card before the plug");
    printf("[64] UAUDIO READY\n"); fflush(stdout);
    CHECK(wait_cards(1, 60), "USB card appeared");
    CHECK(play_blocking("hw:0,0", 48000, 550, 1.0, 0.5, NULL, false) == 0, "550 Hz on the card plugged during the program");
    printf("[64] UAUDIO STAY exiting with the card attached\n"); fflush(stdout);
}
/* Aurora's default-device policy (aurora_sound.c, built standalone) against the real kernel and alsa-lib, set up like
   Aurora does (ALSA_CONFIG_PATH = alsa.conf + the generated file, here in /dev/shm since the test root is read-only):
   one running process, "default" re-opened after every change. HDA only -> HDA; USB plugged (auto) -> USB; explicit
   HDA -> HDA; auto again -> USB; USB unplugged -> HDA; a user-defined default disables ours. The host checks which
   capture each tone landed in. */
#define TEST_CONFIG "/dev/shm/asound.conf"
#define TEST_PREFERENCES "/dev/shm/nexus-sound.conf"
#define TEST_USER "/dev/shm/user-asoundrc"
static void show(const char* what, const struct aurora_sound_state* st) {
    printf("[64] UAUDIO policy %s: output=%s input=%s want=%s/%s cards=%d foreign=%d error=%s\n", what, st->output, st->input,
           st->want_output, st->want_input, st->count, st->foreign, strerror(st->error)); fflush(stdout);
}
static void mode_default(void) {
    setenv("ALSA_CONFIG_PATH", "/usr/share/alsa/alsa.conf:" TEST_CONFIG, 1);
    unlink(TEST_CONFIG); unlink(TEST_PREFERENCES); unlink(TEST_USER);
    const char* const users[] = {TEST_USER, NULL};
    struct aurora_sound_state st; memset(&st, 0, sizeof(st));
    aurora_sound_load(&st, TEST_PREFERENCES, users);
    CHECK(!st.foreign && !strcmp(st.want_output, "auto") && !strcmp(st.want_input, "auto"), "fresh preferences");
    aurora_sound_apply(&st, TEST_CONFIG, TEST_PREFERENCES); show("start", &st);
    CHECK(st.count == 1 && !strcmp(st.output, "NexusHDA") && !strcmp(st.input, "NexusHDA") && !st.error, "HDA only: output %s", st.output);
    CHECK(play_blocking("default", 48000, 440, 1.0, 0.5, NULL, false) == 0, "440 Hz on default (HDA)");
    printf("[64] UAUDIO READY\n"); fflush(stdout);
    CHECK(wait_cards(2, 60), "USB card plugged");
    CHECK(aurora_sound_apply(&st, TEST_CONFIG, TEST_PREFERENCES), "a change"); show("plugged", &st);
    CHECK(!strcmp(st.output, "Audio") && !strcmp(st.input, "NexusHDA"), "auto: newest output (USB) %s, input (no USB capture) %s", st.output, st.input);
    CHECK(play_blocking("default", 48000, 660, 1.0, 0.5, NULL, false) == 0, "660 Hz on default (USB)");
    snprintf(st.want_output, sizeof(st.want_output), "NexusHDA");
    CHECK(aurora_sound_apply(&st, TEST_CONFIG, TEST_PREFERENCES) && !strcmp(st.output, "NexusHDA"), "explicit HDA"); show("explicit", &st);
    CHECK(play_blocking("default", 48000, 880, 1.0, 0.5, NULL, false) == 0, "880 Hz on default (HDA by choice)");
    struct aurora_sound_state reread; memset(&reread, 0, sizeof(reread)); aurora_sound_load(&reread, TEST_PREFERENCES, users);
    CHECK(!strcmp(reread.want_output, "NexusHDA") && !strcmp(reread.want_input, "auto"), "preferences kept: %s/%s", reread.want_output, reread.want_input);
    snprintf(st.want_output, sizeof(st.want_output), "auto");
    CHECK(aurora_sound_apply(&st, TEST_CONFIG, TEST_PREFERENCES) && !strcmp(st.output, "Audio"), "auto again: USB");
    printf("[64] UAUDIO WAIT-UNPLUG\n"); fflush(stdout);
    CHECK(wait_cards(1, 60), "USB card unplugged");
    CHECK(aurora_sound_apply(&st, TEST_CONFIG, TEST_PREFERENCES) && !strcmp(st.output, "NexusHDA"), "fallback to HDA after the unplug: %s", st.output);
    show("unplugged", &st);
    CHECK(play_blocking("default", 48000, 330, 1.0, 0.5, NULL, false) == 0, "330 Hz on default (HDA after unplug)");
    /* a default of the user's own: ours steps back (no pcm.!default written), theirs is used */
    FILE* f = fopen(TEST_USER, "w"); if (f) { fputs("# mine\npcm.!default { type hw card 0 }\n", f); fclose(f); }
    aurora_sound_load(&st, NULL, users); CHECK(st.foreign, "user default recognised");
    aurora_sound_apply(&st, TEST_CONFIG, TEST_PREFERENCES); show("user default", &st);
    char text[512] = ""; f = fopen(TEST_CONFIG, "r"); if (f) { size_t n = fread(text, 1, sizeof(text) - 1, f); text[n] = 0; fclose(f); }
    CHECK(!strstr(text, "pcm.!default") && !st.output[0], "no default of ours while the user has one");
    f = fopen(TEST_USER, "w"); if (f) { fputs("# pcm.!default only in a comment\n", f); fclose(f); }
    aurora_sound_load(&st, NULL, users); CHECK(!st.foreign, "a commented default is not one");
    unlink(TEST_CONFIG); unlink(TEST_PREFERENCES); unlink(TEST_USER);
}
int main(void) {
    setvbuf(stdout, NULL, _IOLBF, 0);
    char mode = 0; FILE* config = fopen("/nexus/test-audio", "r");
    if (config) { mode = (char)fgetc(config); fclose(config); }
    printf("[64] AUDIO TEST START mode=%c\n", mode);
    if (mode == 'u') mode_usb(); else if (mode == 'b') mode_both(); else if (mode == 'h') mode_hotplug(); else if (mode == 's') mode_stay(); else if (mode == 'd') mode_default();
    else CHECK(0, "unknown mode %c", mode);
    printf(failures ? "[64] AUDIO TEST FAILED failures=%d\n" : "[64] AUDIO TEST PASSED failures=%d\n", failures);
    return failures ? 1 : 0;
}
