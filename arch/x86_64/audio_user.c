/* ALSA playback test for the native sound driver (AC'97 or Intel HD Audio), linked against the unmodified Debian
   libasound.so.2. Plays distinct tones per phase so the QEMU WAV capture can be checked offline:
   A 440 Hz 3 s via "default" at 44100 Hz (plug resampling, blocking writei, delay-rate fit)
   B 880 Hz 2 s via "default" at 48000 Hz non-blocking with timer-paced writes (Chrome's pattern)
   C 660 Hz with a forced underrun, -EPIPE and snd_pcm_recover
   D 330 Hz 1 s via "hw:0,0" mmap access, poll()-driven through snd_pcm_wait
   Mode (first byte of /nexus/test-audio): '1' all phases, 'q' 440 Hz, 3 s idle, 660 Hz, 'n' no sound card expected,
   'l'/'s' host latency/stability probes; an optional second byte 'w' delays the start by 5 s. Capture campaign (see
   "capture" below): 'c' counter pattern, 'k' test tone, 'z' silent real input, 'x' no capture device. */
#define _GNU_SOURCE
#include <errno.h>
#include <fcntl.h>
#include <math.h>
#include <dirent.h>
#include <poll.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <sys/sysmacros.h>
#include <sys/wait.h>
#include <signal.h>
#include <unistd.h>

typedef struct _snd_pcm snd_pcm_t;
typedef long snd_pcm_sframes_t;
typedef unsigned long snd_pcm_uframes_t;
typedef struct { void* addr; unsigned int first, step; } snd_pcm_channel_area_t;
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
int snd_pcm_prepare(snd_pcm_t* pcm);
int snd_pcm_pause(snd_pcm_t* pcm, int enable);
int snd_pcm_close(snd_pcm_t* pcm);
int snd_pcm_start(snd_pcm_t* pcm);
int snd_pcm_wait(snd_pcm_t* pcm, int timeout);
int snd_pcm_mmap_begin(snd_pcm_t* pcm, const snd_pcm_channel_area_t** areas, snd_pcm_uframes_t* offset, snd_pcm_uframes_t* frames);
snd_pcm_sframes_t snd_pcm_mmap_commit(snd_pcm_t* pcm, snd_pcm_uframes_t offset, snd_pcm_uframes_t frames);
const char* snd_strerror(int error);
int snd_card_next(int* card);
int snd_card_get_name(int card, char** name);
int snd_card_get_longname(int card, char** name);
int snd_device_name_hint(int card, const char* iface, void*** hints);
char* snd_device_name_get_hint(const void* hint, const char* id);
int snd_device_name_free_hint(void** hints);
enum { PLAYBACK = 0, NONBLOCK = 1, S16_LE = 2, MMAP_INTERLEAVED = 0, RW_INTERLEAVED = 3, STATE_RUNNING = 3, STATE_XRUN = 4 };

static int failures;
#define CHECK(test, ...) do { if (!(test)) { printf("[64] AUDIO FAIL line=%d: ", __LINE__); printf(__VA_ARGS__); printf("\n"); fflush(stdout); failures++; } } while (0)
static double now_s(void) { struct timespec t; clock_gettime(CLOCK_MONOTONIC, &t); return t.tv_sec + t.tv_nsec / 1e9; }
static void sleep_ms(double ms) { struct timespec t = {(time_t)(ms / 1000), (long)(fmod(ms, 1000) * 1e6)}; nanosleep(&t, NULL); }
static void tone(int16_t* out, unsigned frames, double* phase, double frequency, unsigned rate) {
    for (unsigned i = 0; i < frames; i++) {
        int16_t v = (int16_t)lrint(16384.0 * sin(*phase)); out[2 * i] = out[2 * i + 1] = v;
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
    printf("[64] AUDIO open %s rate=%u buffer=%lu period=%lu\n", name, rate, buffer, period); fflush(stdout);
    return pcm;
}
/* Least-squares slope of played frames (written - delay) over time = the device's real consumption rate. */
struct fit { double n, t, f, tt, tf; };
static void fit_add(struct fit* s, double t, double f) { s->n++; s->t += t; s->f += f; s->tt += t * t; s->tf += t * f; }
static double fit_slope(const struct fit* s) { return (s->n * s->tf - s->t * s->f) / (s->n * s->tt - s->t * s->t); }

static bool listed(const char* directory, const char* name) {
    DIR* d = opendir(directory); bool found = false;
    for (struct dirent* e = d ? readdir(d) : NULL; e; e = readdir(d)) if (!strcmp(e->d_name, name)) found = e->d_type == (strcmp(name, "snd") ? DT_CHR : DT_DIR);
    if (d) closedir(d);
    return found;
}
static void enumerate(void) {
    struct stat st;
    CHECK(stat("/dev/snd", &st) == 0 && S_ISDIR(st.st_mode), "/dev/snd is not a directory");
    CHECK(stat("/dev/snd/controlC0", &st) == 0 && S_ISCHR(st.st_mode) && major(st.st_rdev) == 116 && minor(st.st_rdev) == 0, "controlC0 node");
    CHECK(stat("/dev/snd/pcmC0D0p", &st) == 0 && S_ISCHR(st.st_mode) && major(st.st_rdev) == 116 && minor(st.st_rdev) == 16, "pcmC0D0p node");
    CHECK(stat("/dev/snd/controlC1", &st) == -1 && errno == ENOENT, "only one card");
    CHECK(listed("/dev", "snd") && listed("/dev/snd", "controlC0") && listed("/dev/snd", "pcmC0D0p"), "/dev/snd listing");
    int card = -1; CHECK(snd_card_next(&card) == 0 && card == 0, "snd_card_next -> %d", card);
    char* name = NULL; if (card >= 0 && !snd_card_get_name(card, &name)) { printf("[64] AUDIO card0 name=%s\n", name); free(name); }
    name = NULL; if (card >= 0 && !snd_card_get_longname(card, &name)) { printf("[64] AUDIO card0 longname=%s\n", name); free(name); }
    int next = card; snd_card_next(&next); CHECK(next == -1, "only one card expected, next=%d", next);
    void** hints = NULL; int error = snd_device_name_hint(-1, "pcm", &hints);
    CHECK(error == 0, "snd_device_name_hint: %s", snd_strerror(error));
    for (void** h = hints; h && *h; h++) {
        char* n = snd_device_name_get_hint(*h, "NAME"); char* io = snd_device_name_get_hint(*h, "IOID");
        printf("[64] AUDIO hint %s io=%s\n", n ? n : "?", io ? io : "both"); free(n); free(io);
    }
    if (hints) snd_device_name_free_hint(hints);
    fflush(stdout);
}
static bool coarse_position; /* '1w' marker: the device reports its DMA position in coarse steps (VMware) */
static void phase_a(void) {
    snd_pcm_t* pcm = open_pcm("default", 0, RW_INTERLEAVED, 44100, 100000); if (!pcm) return;
    snd_pcm_uframes_t buffer = 0, period = 0; snd_pcm_get_params(pcm, &buffer, &period);
    int16_t chunk[441 * 2]; double phase = 0, start = now_s(), next_sample = start; long written = 0, total = 44100 * 3;
    struct fit rate = {0}; snd_pcm_sframes_t delay_min = 1 << 30, delay_max = 0; int xruns = 0;
    while (written < total) {
        tone(chunk, 441, &phase, 440, 44100);
        snd_pcm_sframes_t n = snd_pcm_writei(pcm, chunk, 441);
        if (n == -EPIPE) { xruns++; snd_pcm_recover(pcm, (int)n, 1); continue; }
        CHECK(n == 441, "writei returned %ld (%s)", n, n < 0 ? snd_strerror((int)n) : "short");
        if (n < 0) break;
        written += n;
        double t = now_s(); snd_pcm_sframes_t delay = 0;
        if (t >= next_sample && snd_pcm_state(pcm) == STATE_RUNNING && snd_pcm_delay(pcm, &delay) == 0) {
            next_sample = t + 0.05; fit_add(&rate, t - start, (double)(written - delay));
            if (delay < delay_min) delay_min = delay;
            if (delay > delay_max) delay_max = delay;
        }
    }
    double before_drain = now_s(); int error = snd_pcm_drain(pcm); double drained = now_s();
    CHECK(error == 0, "drain: %s", snd_strerror(error));
    double slope = rate.n > 4 ? fit_slope(&rate) : 0;
    printf("[64] AUDIO A frames=%ld xruns=%d rate_fit=%.1f delay_min=%ld delay_max=%ld samples=%.0f drain_ms=%.1f elapsed=%.3f\n",
           written, xruns, slope, delay_min, delay_max, rate.n, (drained - before_drain) * 1000, drained - start);
    CHECK(xruns == 0, "phase A underruns: %d", xruns);
    if (coarse_position) {
        double played = (double)written / (drained - start);   /* every frame was played once drain returned */
        CHECK(played > 44100 * 0.97 && played < 44100 * 1.03 && slope > 44100 * 0.94 && slope < 44100 * 1.06,
              "consumption rate %.1f frames/s over the whole phase, fit %.1f", played, slope);
    } else CHECK(slope > 44100 * 0.97 && slope < 44100 * 1.03, "consumption rate %.1f frames/s", slope);
    CHECK(delay_max <= (snd_pcm_sframes_t)buffer + 512 && delay_min >= 0, "delay outside the %lu-frame buffer: %ld..%ld", buffer, delay_min, delay_max);
    snd_pcm_close(pcm);
}
static void phase_b(void) {
    snd_pcm_t* pcm = open_pcm("default", NONBLOCK, RW_INTERLEAVED, 48000, 40000); if (!pcm) return;
    snd_pcm_uframes_t buffer = 0, period = 0; snd_pcm_get_params(pcm, &buffer, &period);
    int16_t* data = calloc(buffer * 2, sizeof(int16_t)); double phase = 0, start = now_s();
    long written = 0, total = 48000 * 2; int again = 0, xruns = 0, writes = 0;
    while (written < total) {
        snd_pcm_sframes_t space = snd_pcm_avail_update(pcm);
        if (space == -EPIPE) { xruns++; snd_pcm_recover(pcm, -EPIPE, 1); continue; }
        CHECK(space >= 0, "avail_update: %s", snd_strerror((int)space));
        if (space < 0) break;
        if (space > total - written) space = total - written;
        if (space > 0) {
            tone(data, (unsigned)space, &phase, 880, 48000);
            snd_pcm_sframes_t n = snd_pcm_writei(pcm, data, (snd_pcm_uframes_t)space);
            if (n == -EAGAIN) { again++; n = 0; }
            else if (n == -EPIPE) { xruns++; snd_pcm_recover(pcm, -EPIPE, 1); continue; }
            CHECK(n >= 0, "nonblocking writei: %s", snd_strerror((int)n));
            if (n < 0) break;
            if (n < space) phase -= 2 * M_PI * 880.0 / 48000 * (double)(space - n);
            written += n; writes++;
        }
        sleep_ms(10);
    }
    snd_pcm_sframes_t delay = 0; snd_pcm_delay(pcm, &delay);
    double wait_until = now_s() + (double)delay / 48000 + 0.02;
    while (now_s() < wait_until) sleep_ms(5);
    snd_pcm_sframes_t space = snd_pcm_avail_update(pcm);
    printf("[64] AUDIO B frames=%ld writes=%d eagain=%d xruns=%d buffer=%lu final_avail=%ld state=%d elapsed=%.3f\n",
           written, writes, again, xruns, buffer, space, snd_pcm_state(pcm), now_s() - start);
    CHECK(xruns == 0, "phase B underruns while timer-fed: %d", xruns);
    CHECK(space == -EPIPE || snd_pcm_state(pcm) == STATE_XRUN, "exhausted stream must report an xrun (got %ld)", space);
    snd_pcm_drop(pcm); snd_pcm_close(pcm); free(data);
}
static void phase_c(void) {
    snd_pcm_t* pcm = open_pcm("default", 0, RW_INTERLEAVED, 48000, 60000); if (!pcm) return;
    int16_t chunk[480 * 2]; double phase = 0; int epipe = 0, recovered = 0;
    for (int i = 0; i < 50; i++) { tone(chunk, 480, &phase, 660, 48000); CHECK(snd_pcm_writei(pcm, chunk, 480) == 480, "phase C write"); }
    sleep_ms(300);
    CHECK(snd_pcm_state(pcm) == STATE_XRUN || snd_pcm_avail_update(pcm) == -EPIPE, "starved stream not in XRUN (state %d)", snd_pcm_state(pcm));
    for (int i = 0; i < 50;) {
        tone(chunk, 480, &phase, 660, 48000);
        snd_pcm_sframes_t n = snd_pcm_writei(pcm, chunk, 480);
        if (n == -EPIPE) { epipe++; if (snd_pcm_recover(pcm, (int)n, 1) == 0) recovered++; phase -= 2 * M_PI * 660.0 / 48000 * 480; continue; }
        CHECK(n == 480, "phase C write after recovery: %ld", n); i++;
    }
    CHECK(snd_pcm_drain(pcm) == 0, "phase C drain");
    printf("[64] AUDIO C epipe=%d recovered=%d\n", epipe, recovered);
    CHECK(epipe >= 1 && recovered == epipe, "expected -EPIPE then snd_pcm_recover");
    snd_pcm_close(pcm);
}
static void phase_d(void) {
    snd_pcm_t* pcm = open_pcm("hw:0,0", NONBLOCK, MMAP_INTERLEAVED, 48000, 50000); if (!pcm) return;
    double phase = 0; long written = 0, total = 48000; int waits = 0, timeouts = 0; bool started = false;
    while (written < total) {
        snd_pcm_sframes_t space = snd_pcm_avail_update(pcm);
        CHECK(space >= 0, "hw avail_update: %s", snd_strerror((int)space));
        if (space < 0) break;
        if (space == 0) {
            if (!started) { CHECK(snd_pcm_start(pcm) == 0, "explicit start"); started = true; continue; }
            int ready = snd_pcm_wait(pcm, 1000); waits++;
            if (ready == 0) timeouts++;
            CHECK(ready >= 0, "snd_pcm_wait: %s", snd_strerror(ready)); continue;
        }
        const snd_pcm_channel_area_t* areas; snd_pcm_uframes_t offset, frames = (snd_pcm_uframes_t)space;
        if (frames > (snd_pcm_uframes_t)(total - written)) frames = total - written;
        int error = snd_pcm_mmap_begin(pcm, &areas, &offset, &frames);
        CHECK(error == 0, "mmap_begin: %s", snd_strerror(error)); if (error) break;
        int16_t* base = (int16_t*)((char*)areas[0].addr + areas[0].first / 8 + offset * areas[0].step / 8);
        CHECK(areas[0].step == 32 && areas[1].first == 16, "interleaved S16 stereo areas");
        if (!written) { errno = 0; CHECK(madvise((void*)((uintptr_t)areas[0].addr & ~(uintptr_t)4095), 4096, MADV_REMOVE) == -1 && errno == ENODEV, "MADV_REMOVE on the PCM mapping: %s", strerror(errno)); }
        tone(base, (unsigned)frames, &phase, 330, 48000);
        CHECK(snd_pcm_mmap_commit(pcm, offset, frames) == (snd_pcm_sframes_t)frames, "mmap_commit");
        written += frames;
    }
    if (!started) snd_pcm_start(pcm);
    snd_pcm_sframes_t delay = 0; snd_pcm_delay(pcm, &delay);
    sleep_ms((double)delay * 1000 / 48000 + 20);
    printf("[64] AUDIO D frames=%ld waits=%d timeouts=%d final_delay=%ld\n", written, waits, timeouts, delay);
    CHECK(waits > 0 && timeouts == 0, "poll readiness never woke the writer");
    snd_pcm_drop(pcm); snd_pcm_close(pcm);
}
/* Two clients at once (Chrome opens one stream per sink): 1000 Hz and 1500 Hz at 0.25 each must both be audible. */
static void phase_e(void) {
    snd_pcm_t* a = open_pcm("default", NONBLOCK, RW_INTERLEAVED, 48000, 80000);
    snd_pcm_t* b = open_pcm("default", NONBLOCK, RW_INTERLEAVED, 48000, 80000);
    CHECK(a && b, "second concurrent open failed"); if (!a || !b) { if (a) snd_pcm_close(a); if (b) snd_pcm_close(b); return; }
    int16_t* data = calloc(48000 * 2, sizeof(int16_t)); double pa = 0, pb = 0, start = now_s(); int xruns = 0; long wa = 0, wb = 0;
    while (now_s() - start < 2.5) {
        snd_pcm_t* pcms[2] = {a, b}; double* phases[2] = {&pa, &pb}; long* written[2] = {&wa, &wb}; const double frequency[2] = {1000, 1500};
        for (int k = 0; k < 2; k++) {
            if (k == 1 && now_s() - start > 2.0) continue; /* b stops first; a continues alone */
            snd_pcm_sframes_t space = snd_pcm_avail_update(pcms[k]);
            if (space < 0) { xruns++; snd_pcm_recover(pcms[k], (int)space, 1); continue; }
            if (!space) continue;
            for (snd_pcm_sframes_t i = 0; i < space; i++) {
                int16_t v = (int16_t)lrint(8192.0 * sin(*phases[k])); data[2 * i] = data[2 * i + 1] = v;
                *phases[k] += 2 * M_PI * frequency[k] / 48000; if (*phases[k] > 2 * M_PI) *phases[k] -= 2 * M_PI;
            }
            snd_pcm_sframes_t n = snd_pcm_writei(pcms[k], data, (snd_pcm_uframes_t)space);
            CHECK(n == space, "concurrent write %d: %ld", k, n);
            if (n > 0) *written[k] += n;
        }
        if (now_s() - start > 2.0 && b) { snd_pcm_drop(b); snd_pcm_close(b); b = NULL; }
        sleep_ms(10);
    }
    snd_pcm_drain(a); snd_pcm_close(a); if (b) snd_pcm_close(b); free(data);
    printf("[64] AUDIO E frames_a=%ld frames_b=%ld xruns=%d\n", wa, wb, xruns);
    CHECK(xruns == 0, "concurrent streams underran: %d", xruns);
}
/* Pause/resume: 550 Hz, 0.3 s paused; the pause must leave silence (no stale audio) and resume where it stopped. */
static void phase_f(void) {
    snd_pcm_t* pcm = open_pcm("default", NONBLOCK, RW_INTERLEAVED, 48000, 80000); if (!pcm) return;
    int16_t data[480 * 2]; double phase = 0, start = now_s(); bool paused = false, resumed = false; long written = 0;
    while (now_s() - start < 1.6) {
        double t = now_s() - start;
        if (!paused && t > 0.6) { CHECK(snd_pcm_pause(pcm, 1) == 0, "pause"); paused = true; }
        if (paused && !resumed && t > 0.9) { CHECK(snd_pcm_pause(pcm, 0) == 0, "resume"); resumed = true; }
        if (paused && !resumed) { sleep_ms(5); continue; }
        snd_pcm_sframes_t space = snd_pcm_avail_update(pcm);
        if (space < 0) { CHECK(0, "phase F xrun %ld", space); snd_pcm_recover(pcm, (int)space, 1); continue; }
        while (space > 0) {
            unsigned chunk = space > 480 ? 480 : (unsigned)space; tone(data, chunk, &phase, 550, 48000);
            CHECK(snd_pcm_writei(pcm, data, chunk) == (snd_pcm_sframes_t)chunk, "phase F write"); written += chunk; space -= chunk;
        }
        sleep_ms(5);
    }
    snd_pcm_drain(pcm); snd_pcm_close(pcm);
    printf("[64] AUDIO F frames=%ld paused=%d resumed=%d\n", written, paused, resumed);
}
/* Host-side buffering probe: a full 1 s buffer auto-starts at t=0; played(t) - 48000*t averaged after the start-up
   transient is the audio QEMU accepted ahead of real time (mixing buffer + backend queue = added output latency). */
static int compare(const void* a, const void* b) { double x = *(const double*)a, y = *(const double*)b; return (x > y) - (x < y); }
static void probe_latency(void) {
    snd_pcm_t* pcm = open_pcm("hw:0,0", NONBLOCK, RW_INTERLEAVED, 48000, 1000000); if (!pcm) return;
    snd_pcm_uframes_t buffer = 0, period = 0; snd_pcm_get_params(pcm, &buffer, &period);
    int16_t* data = calloc(buffer * 2, sizeof(int16_t)); double phase = 0, ahead[512];
    tone(data, (unsigned)buffer, &phase, 440, 48000);
    for (int round = 0; round < 3; round++) {
        CHECK(snd_pcm_writei(pcm, data, buffer) == (snd_pcm_sframes_t)buffer, "probe fill");
        double start = now_s(); snd_pcm_sframes_t delay = 0, first_played = 0; double first_t = -1; int count = 0;
        CHECK(snd_pcm_state(pcm) == STATE_RUNNING, "full buffer did not start the stream");
        while (count < 512) {
            if (snd_pcm_delay(pcm, &delay)) break;
            double t = now_s() - start; snd_pcm_sframes_t played = (snd_pcm_sframes_t)buffer - delay;
            if (first_t < 0 && played > 0) { first_t = t; first_played = played; }
            if (t > 0.2) ahead[count++] = (double)played - 48000.0 * t;
            if (t > 0.85) break;
            sleep_ms(2);
        }
        qsort(ahead, count, sizeof(double), compare);
        printf("[64] AUDIO PROBE round=%d first_consume_ms=%.2f first_frames=%ld ahead_ms_p10=%.1f median=%.1f p90=%.1f samples=%d\n",
               round, first_t * 1000, first_played, ahead[count / 10] / 48, ahead[count / 2] / 48, ahead[count * 9 / 10] / 48, count);
        snd_pcm_drop(pcm); snd_pcm_prepare(pcm); sleep_ms(200);
    }
    free(data); snd_pcm_close(pcm);
}
/* Steady-state probe: keep ~100 ms queued for 20 s; QEMU pulls whatever the host backend accepts, so bursts in
   played(t) - rate*t and long gaps without consumption expose host-side underruns/stalls. */
static void probe_stability(void) {
    snd_pcm_t* pcm = open_pcm("hw:0,0", NONBLOCK, RW_INTERLEAVED, 48000, 100000); if (!pcm) return;
    snd_pcm_uframes_t buffer = 0, period = 0; snd_pcm_get_params(pcm, &buffer, &period);
    int16_t* data = calloc(buffer * 2, sizeof(int16_t)); double phase = 0, start = -1, last_change = 0, longest = 0;
    static double t_samples[8000], p_samples[8000]; int count = 0, xruns = 0; long written = 0; snd_pcm_sframes_t last_played = -1;
    while (count < 8000) {
        snd_pcm_sframes_t space = snd_pcm_avail_update(pcm);
        if (space < 0) { xruns++; snd_pcm_recover(pcm, (int)space, 1); continue; }
        if (space > 0) { tone(data, (unsigned)space, &phase, 440, 48000); snd_pcm_sframes_t n = snd_pcm_writei(pcm, data, space); if (n > 0) written += n; }
        snd_pcm_sframes_t delay = 0;
        if (snd_pcm_state(pcm) == STATE_RUNNING && snd_pcm_delay(pcm, &delay) == 0) {
            double t = now_s(); if (start < 0) { start = t; last_change = t; }
            snd_pcm_sframes_t played = written - delay;
            if (played != last_played) { if (t - last_change > longest) longest = t - last_change; last_change = t; last_played = played; }
            t_samples[count] = t - start; p_samples[count] = (double)played; count++;
            if (t - start > 20) break;
        }
        sleep_ms(4);
    }
    struct fit line = {0}; for (int i = count / 10; i < count; i++) fit_add(&line, t_samples[i], p_samples[i]);
    double slope = fit_slope(&line), intercept = (line.f - slope * line.t) / line.n, low = 1e9, high = -1e9;
    for (int i = count / 10; i < count; i++) { double e = p_samples[i] - (intercept + slope * t_samples[i]); if (e < low) low = e; if (e > high) high = e; }
    printf("[64] AUDIO STABILITY rate=%.1f jitter_ms=%.1f..%.1f longest_no_consume_ms=%.1f xruns=%d samples=%d\n",
           slope, low / 48, high / 48, longest * 1000, xruns, count);
    free(data); snd_pcm_drop(pcm); snd_pcm_close(pcm);
}
/* Device bring-up checks: a clean 440 Hz tone (1 s via "default" at 48 kHz, blocking writes), 3 s of idle (long
   enough for the driver to release an idle DMA engine), then a clean 660 Hz tone through a restarted engine. */
static void quick(void) {
    const double frequencies[2] = {440, 660};
    for (int k = 0; k < 2; k++) {
        if (k) sleep_ms(3000);
        snd_pcm_t* pcm = open_pcm("default", 0, RW_INTERLEAVED, 48000, 100000); if (!pcm) return;
        int16_t chunk[480 * 2]; double phase = 0; int xruns = 0;
        for (int i = 0; i < 100; i++) {
            tone(chunk, 480, &phase, frequencies[k], 48000);
            snd_pcm_sframes_t n = snd_pcm_writei(pcm, chunk, 480);
            if (n == -EPIPE) { xruns++; snd_pcm_recover(pcm, (int)n, 1); continue; }
            CHECK(n == 480, "quick writei returned %ld", n);
        }
        CHECK(snd_pcm_drain(pcm) == 0, "quick drain");
        printf("[64] AUDIO Q tone=%.0f frames=48000 xruns=%d\n", frequencies[k], xruns);
        CHECK(xruns == 0, "quick tone underran: %d", xruns);
        snd_pcm_close(pcm);
    }
}
/* No sound card (no audio device, or an HD Audio controller without a codec): the device nodes are absent and every
   open fails cleanly with an error instead of hanging or crashing. */
static void no_card(void) {
    struct stat st;
    CHECK(stat("/dev/snd", &st) == -1 && errno == ENOENT, "/dev/snd must not exist without a card");
    CHECK(stat("/dev/snd/controlC0", &st) == -1 && errno == ENOENT, "controlC0 must not exist");
    CHECK(open("/dev/snd/pcmC0D0p", O_WRONLY | O_NONBLOCK) == -1 && errno == ENOENT, "pcmC0D0p open must fail with ENOENT");
    int card = -1, error = snd_card_next(&card);
    CHECK(error == 0 && card == -1, "snd_card_next -> %d (%s)", card, snd_strerror(error));
    const char* names[2] = {"hw:0,0", "default"};
    for (int i = 0; i < 2; i++) {
        snd_pcm_t* pcm = NULL; error = snd_pcm_open(&pcm, names[i], PLAYBACK, NONBLOCK);
        printf("[64] AUDIO nocard open %s -> %d (%s)\n", names[i], error, error < 0 ? snd_strerror(error) : "opened");
        CHECK(error < 0, "%s open must fail without a card", names[i]);
        if (!error && pcm) snd_pcm_close(pcm);
    }
}
/* ---------- capture (pcmC0D0c) ----------
   Modes: 'c' the kernel's counter test pattern (snd.test-capture=counter: left/right = low/high 16 bits of the device
   frame number, so every gap, repeat or splice is visible), 'k' its 1 kHz tone (snd.test-capture=tone), 'z' the real
   input of a VM whose audio backend records silence ("none"), 'x' a card without a capture device. */
typedef struct _snd_pcm_hw_params snd_pcm_hw_params_t;
typedef struct _snd_mixer snd_mixer_t;
typedef struct _snd_mixer_elem snd_mixer_elem_t;
typedef struct _snd_pcm_info snd_pcm_info_t;
snd_pcm_sframes_t snd_pcm_readi(snd_pcm_t* pcm, void* buffer, snd_pcm_uframes_t size);
snd_pcm_sframes_t snd_pcm_avail(snd_pcm_t* pcm);
int snd_pcm_hw_params_malloc(snd_pcm_hw_params_t** params);
void snd_pcm_hw_params_free(snd_pcm_hw_params_t* params);
int snd_pcm_hw_params_any(snd_pcm_t* pcm, snd_pcm_hw_params_t* params);
int snd_pcm_hw_params_set_access(snd_pcm_t* pcm, snd_pcm_hw_params_t* params, int access);
int snd_pcm_hw_params_set_format(snd_pcm_t* pcm, snd_pcm_hw_params_t* params, int format);
int snd_pcm_hw_params_set_channels(snd_pcm_t* pcm, snd_pcm_hw_params_t* params, unsigned channels);
int snd_pcm_hw_params_set_rate(snd_pcm_t* pcm, snd_pcm_hw_params_t* params, unsigned rate, int dir);
int snd_pcm_hw_params_set_period_size_near(snd_pcm_t* pcm, snd_pcm_hw_params_t* params, snd_pcm_uframes_t* frames, int* dir);
int snd_pcm_hw_params_set_buffer_size_near(snd_pcm_t* pcm, snd_pcm_hw_params_t* params, snd_pcm_uframes_t* frames);
int snd_pcm_hw_params_can_pause(const snd_pcm_hw_params_t* params);
int snd_pcm_hw_params(snd_pcm_t* pcm, snd_pcm_hw_params_t* params);
int snd_pcm_poll_descriptors_count(snd_pcm_t* pcm);
int snd_pcm_poll_descriptors(snd_pcm_t* pcm, struct pollfd* fds, unsigned space);
int snd_pcm_poll_descriptors_revents(snd_pcm_t* pcm, struct pollfd* fds, unsigned count, unsigned short* revents);
int snd_pcm_info_malloc(snd_pcm_info_t** info);
void snd_pcm_info_free(snd_pcm_info_t* info);
int snd_pcm_info(snd_pcm_t* pcm, snd_pcm_info_t* info);
const char* snd_pcm_info_get_name(const snd_pcm_info_t* info);
int snd_pcm_info_get_stream(const snd_pcm_info_t* info);
int snd_mixer_open(snd_mixer_t** mixer, int mode);
int snd_mixer_attach(snd_mixer_t* mixer, const char* name);
int snd_mixer_selem_register(snd_mixer_t* mixer, void* options, void* classp);
int snd_mixer_load(snd_mixer_t* mixer);
int snd_mixer_close(snd_mixer_t* mixer);
snd_mixer_elem_t* snd_mixer_first_elem(snd_mixer_t* mixer);
snd_mixer_elem_t* snd_mixer_elem_next(snd_mixer_elem_t* elem);
const char* snd_mixer_selem_get_name(snd_mixer_elem_t* elem);
int snd_mixer_selem_has_capture_volume(snd_mixer_elem_t* elem);
int snd_mixer_selem_has_capture_switch(snd_mixer_elem_t* elem);
int snd_mixer_selem_get_capture_volume_range(snd_mixer_elem_t* elem, long* min, long* max);
int snd_mixer_selem_get_capture_dB_range(snd_mixer_elem_t* elem, long* min, long* max);
int snd_mixer_selem_set_capture_volume_all(snd_mixer_elem_t* elem, long value);
int snd_mixer_selem_get_capture_volume(snd_mixer_elem_t* elem, int channel, long* value);
int snd_mixer_selem_set_capture_switch_all(snd_mixer_elem_t* elem, int value);
int snd_mixer_selem_get_capture_switch(snd_mixer_elem_t* elem, int channel, int* value);
int snd_mixer_selem_get_capture_dB(snd_mixer_elem_t* elem, int channel, long* value);
enum { CAPTURE = 1, STATE_PREPARED = 2, STATE_SETUP = 1, STATE_PAUSED = 6 };

static char capture_mode;
/* Counter-pattern continuity: frame value = (uint16 left) | (uint16 right) << 16 must step by one. */
struct counter { bool started; uint32_t next; long frames, gaps, jump; };
static void counter_check(struct counter* c, const int16_t* data, long frames) {
    for (long i = 0; i < frames; i++) {
        uint32_t value = (uint16_t)data[2 * i] | (uint32_t)(uint16_t)data[2 * i + 1] << 16;
        if (c->started && value != c->next) { c->gaps++; c->jump = (long)(value - c->next); }
        c->started = true; c->next = value + 1; c->frames++;
    }
}
static snd_pcm_t* open_capture(const char* name, int mode, unsigned channels, unsigned rate, unsigned latency) {
    snd_pcm_t* pcm = NULL; int error = snd_pcm_open(&pcm, name, CAPTURE, mode);
    CHECK(error == 0, "capture open(%s): %s", name, snd_strerror(error));
    if (error) return NULL;
    error = snd_pcm_set_params(pcm, S16_LE, RW_INTERLEAVED, channels, rate, 1, latency);
    CHECK(error == 0, "capture set_params(%s, %u ch, %u Hz): %s", name, channels, rate, snd_strerror(error));
    if (error) { snd_pcm_close(pcm); return NULL; }
    snd_pcm_uframes_t buffer = 0, period = 0; snd_pcm_get_params(pcm, &buffer, &period);
    printf("[64] AUDIO capture open %s ch=%u rate=%u buffer=%lu period=%lu\n", name, channels, rate, buffer, period); fflush(stdout);
    return pcm;
}
/* Raw hw:0,0 capture with explicit hw_params (access, period/buffer). */
static snd_pcm_t* open_capture_hw(int mode, int access, snd_pcm_uframes_t period, snd_pcm_uframes_t buffer, bool* can_pause) {
    snd_pcm_t* pcm = NULL; snd_pcm_hw_params_t* p = NULL; int error = snd_pcm_open(&pcm, "hw:0,0", CAPTURE, mode);
    CHECK(error == 0, "hw:0,0 capture open: %s", snd_strerror(error));
    if (error) return NULL;
    snd_pcm_hw_params_malloc(&p); int dir = 0;
    error = snd_pcm_hw_params_any(pcm, p);
    if (!error) error = snd_pcm_hw_params_set_access(pcm, p, access);
    if (!error) error = snd_pcm_hw_params_set_format(pcm, p, S16_LE);
    if (!error) error = snd_pcm_hw_params_set_channels(pcm, p, 2);
    if (!error) error = snd_pcm_hw_params_set_rate(pcm, p, 48000, 0);
    if (!error) error = snd_pcm_hw_params_set_period_size_near(pcm, p, &period, &dir);
    if (!error) error = snd_pcm_hw_params_set_buffer_size_near(pcm, p, &buffer);
    if (!error) error = snd_pcm_hw_params(pcm, p);
    if (can_pause) *can_pause = !error && snd_pcm_hw_params_can_pause(p);
    snd_pcm_hw_params_free(p);
    CHECK(error == 0, "hw:0,0 capture hw_params: %s", snd_strerror(error));
    if (error) { snd_pcm_close(pcm); return NULL; }
    printf("[64] AUDIO capture hw access=%d period=%lu buffer=%lu\n", access, period, buffer); fflush(stdout);
    return pcm;
}
static void capture_enumerate(void) {
    struct stat st;
    CHECK(stat("/dev/snd/pcmC0D0c", &st) == 0 && S_ISCHR(st.st_mode) && major(st.st_rdev) == 116 && minor(st.st_rdev) == 24, "pcmC0D0c node");
    CHECK(listed("/dev/snd", "pcmC0D0c"), "pcmC0D0c listed");
    void** hints = NULL; bool input = false;
    if (!snd_device_name_hint(-1, "pcm", &hints))
        for (void** h = hints; h && *h; h++) { char* io = snd_device_name_get_hint(*h, "IOID"); input |= !io || !strcmp(io, "Input"); free(io); }
    if (hints) snd_device_name_free_hint(hints);
    CHECK(input, "no capture-capable PCM in the device hints");
    snd_pcm_t* pcm = NULL; snd_pcm_info_t* info = NULL;
    if (!snd_pcm_open(&pcm, "hw:0,0", CAPTURE, NONBLOCK)) {
        snd_pcm_info_malloc(&info);
        CHECK(snd_pcm_info(pcm, info) == 0 && snd_pcm_info_get_stream(info) == CAPTURE, "capture PCM info");
        printf("[64] AUDIO capture device name=%s\n", snd_pcm_info_get_name(info)); snd_pcm_info_free(info); snd_pcm_close(pcm);
    } else CHECK(false, "hw:0,0 capture open failed");
    fflush(stdout);
}
/* CA: "default", blocking readi, 1.5 s: continuity (counter) and the capture rate from the frame count over time. */
static void capture_blocking(void) {
    snd_pcm_t* pcm = open_capture("default", 0, 2, 48000, 100000); if (!pcm) return;
    /* snd_pcm_set_params puts start_threshold at the buffer size, so (as on Linux) a smaller readi would wait for
       someone to start the stream: start it, as Chrome does */
    CHECK(snd_pcm_start(pcm) == 0, "capture start");
    int16_t chunk[480 * 2]; struct counter c = {0}; long total = 0; double start = 0; int xruns = 0; bool silent = true; int peak = 0;
    double sum_rate = 0; long zero_crossings = 0; int16_t last = 0;
    while (total < 48000 * 3 / 2) {
        snd_pcm_sframes_t n = snd_pcm_readi(pcm, chunk, 480);
        if (n == -EPIPE) { xruns++; snd_pcm_recover(pcm, (int)n, 1); snd_pcm_start(pcm); continue; }
        CHECK(n == 480, "capture readi returned %ld (%s)", n, n < 0 ? snd_strerror((int)n) : "short");
        if (n <= 0) break;
        if (!total) start = now_s();                      /* first block: recording started (start_threshold) */
        total += n;
        if (capture_mode == 'c') counter_check(&c, chunk, n);
        for (long i = 0; i < 2 * n; i++) { int v = abs(chunk[i]); if (v > peak) peak = v; silent &= !chunk[i]; }
        for (long i = 0; i < n; i++) { if ((last < 0) != (chunk[2 * i] < 0)) zero_crossings++; last = chunk[2 * i]; }
    }
    double elapsed = now_s() - start; sum_rate = (total - 480) / elapsed;
    printf("[64] AUDIO CA frames=%ld xruns=%d rate=%.1f gaps=%ld peak=%d crossings=%ld\n", total, xruns, sum_rate, c.gaps, peak, zero_crossings);
    CHECK(xruns == 0, "capture overruns: %d", xruns);
    CHECK(sum_rate > 48000 * 0.97 && sum_rate < 48000 * 1.03, "capture rate %.1f frames/s", sum_rate);
    if (capture_mode == 'c') CHECK(c.gaps == 0 && c.frames == total, "counter pattern gaps=%ld (last jump %ld)", c.gaps, c.jump);
    if (capture_mode == 'z') CHECK(silent, "silent input expected, peak %d", peak);
    if (capture_mode == 'k') {
        double frequency = zero_crossings / 2.0 / (total / 48000.0);
        CHECK(fabs(frequency - 1000) < 5 && peak >= 8000 && peak <= 8192, "1 kHz test tone: %.1f Hz, peak %d", frequency, peak);
    }
    CHECK(snd_pcm_drop(pcm) == 0, "capture drop");
    snd_pcm_close(pcm);
}
/* CB: Chrome's pattern: non-blocking "default", 44.1 kHz mono through the plug (rate + channel conversion), paced by
   10 ms timers with avail_update; plus the delay report. */
static void capture_chrome_pattern(void) {
    snd_pcm_t* pcm = open_capture("default", NONBLOCK, 1, 44100, 20000); if (!pcm) return;
    int16_t chunk[4410]; long total = 0; int xruns = 0, again = 0; double start = now_s(); long zero_crossings = 0; int16_t last = 0;
    CHECK(snd_pcm_start(pcm) == 0, "capture start");
    snd_pcm_sframes_t delay_max = 0;
    while (now_s() - start < 1.5) {
        sleep_ms(10);
        snd_pcm_sframes_t avail = snd_pcm_avail_update(pcm);
        if (avail == -EPIPE) { xruns++; snd_pcm_recover(pcm, (int)avail, 1); snd_pcm_start(pcm); continue; }
        CHECK(avail >= 0, "avail_update: %s", snd_strerror((int)avail));
        if (avail <= 0) continue;
        if (avail > 4410) avail = 4410;
        snd_pcm_sframes_t n = snd_pcm_readi(pcm, chunk, (snd_pcm_uframes_t)avail);
        if (n == -EAGAIN) { again++; continue; }
        if (n == -EPIPE) { xruns++; snd_pcm_recover(pcm, (int)n, 1); snd_pcm_start(pcm); continue; }
        CHECK(n > 0, "non-blocking readi: %s", snd_strerror((int)n));
        if (n <= 0) break;
        total += n;
        for (long i = 0; i < n; i++) { if ((last < 0) != (chunk[i] < 0)) zero_crossings++; last = chunk[i]; }
        snd_pcm_sframes_t delay = 0; if (!snd_pcm_delay(pcm, &delay) && delay > delay_max) delay_max = delay;
    }
    double elapsed = now_s() - start, rate = total / elapsed;
    printf("[64] AUDIO CB frames=%ld rate=%.1f xruns=%d eagain=%d delay_max=%ld crossings=%ld\n", total, rate, xruns, again, delay_max, zero_crossings);
    CHECK(xruns == 0 && rate > 44100 * 0.95 && rate < 44100 * 1.03, "44.1 kHz mono capture: rate %.1f, xruns %d", rate, xruns);
    if (capture_mode == 'k') { double f = zero_crossings / 2.0 / (total / 44100.0); CHECK(fabs(f - 1000) < 10, "resampled tone %.1f Hz", f); }
    snd_pcm_close(pcm);
}
/* CC: hw:0,0 mmap capture, poll()-driven through the PCM's poll descriptors; snd_pcm_wait between blocks. */
static void capture_mmap(void) {
    snd_pcm_t* pcm = open_capture_hw(NONBLOCK, MMAP_INTERLEAVED, 1024, 4096, NULL); if (!pcm) return;
    struct counter c = {0}; long total = 0; int polls = 0, xruns = 0;
    int count = snd_pcm_poll_descriptors_count(pcm); struct pollfd fds[4];
    CHECK(count >= 1 && count <= 4 && snd_pcm_poll_descriptors(pcm, fds, (unsigned)count) == count, "poll descriptors");
    CHECK(snd_pcm_start(pcm) == 0, "mmap capture start");
    double start = now_s();
    while (now_s() - start < 1.0 && count >= 1) {
        int ready = poll(fds, (nfds_t)count, 500); unsigned short revents = 0;
        CHECK(ready > 0, "capture poll timed out"); if (ready <= 0) break;
        polls++; snd_pcm_poll_descriptors_revents(pcm, fds, (unsigned)count, &revents);
        if (revents & POLLERR) { xruns++; snd_pcm_recover(pcm, -EPIPE, 1); snd_pcm_start(pcm); continue; }
        if (!(revents & POLLIN)) continue;
        snd_pcm_sframes_t avail = snd_pcm_avail_update(pcm);
        while (avail > 0) {
            const snd_pcm_channel_area_t* areas; snd_pcm_uframes_t offset, frames = (snd_pcm_uframes_t)avail;
            int error = snd_pcm_mmap_begin(pcm, &areas, &offset, &frames);
            CHECK(error == 0, "mmap_begin: %s", snd_strerror(error)); if (error) break;
            const int16_t* data = (const int16_t*)((const char*)areas[0].addr + areas[0].first / 8 + offset * areas[0].step / 8);
            if (capture_mode == 'c') counter_check(&c, data, (long)frames);
            CHECK(snd_pcm_mmap_commit(pcm, offset, frames) == (snd_pcm_sframes_t)frames, "mmap_commit");
            total += (long)frames; avail -= (snd_pcm_sframes_t)frames;
        }
    }
    double rate = total / (now_s() - start);
    printf("[64] AUDIO CC frames=%ld polls=%d xruns=%d rate=%.1f gaps=%ld\n", total, polls, xruns, rate, c.gaps);
    CHECK(xruns == 0 && rate > 48000 * 0.9 && rate < 48000 * 1.05 && polls >= 20, "mmap capture: rate %.1f, polls %d, xruns %d", rate, polls, xruns);
    if (capture_mode == 'c') CHECK(c.gaps == 0, "mmap counter gaps=%ld jump=%ld", c.gaps, c.jump);
    snd_pcm_close(pcm);
}
/* CD: overrun -> -EPIPE, XRUN state, snd_pcm_recover, recording resumes (the counter jumps once, then is continuous);
   pause/resume (frames during the pause are not recorded); drain (captured data stays readable, then SETUP). */
static void capture_xrun_pause_drain(void) {
    bool can_pause = false;
    snd_pcm_t* pcm = open_capture_hw(0, RW_INTERLEAVED, 1024, 4096, &can_pause); if (!pcm) return;
    int16_t chunk[1024 * 2]; struct counter c = {0};
    CHECK(snd_pcm_readi(pcm, chunk, 1024) == 1024, "first read"); counter_check(&c, chunk, 1024);
    sleep_ms(300);                                          /* 14400 frames > the 4096-frame buffer */
    snd_pcm_sframes_t n = snd_pcm_readi(pcm, chunk, 1024);
    CHECK(n == -EPIPE && snd_pcm_state(pcm) == STATE_XRUN, "overrun: readi %ld state %d", n, snd_pcm_state(pcm));
    CHECK(snd_pcm_recover(pcm, (int)n, 1) == 0, "recover");
    long gaps_before = c.gaps;
    for (int i = 0; i < 20; i++) { n = snd_pcm_readi(pcm, chunk, 1024); CHECK(n == 1024, "read after recover %ld", n); if (n != 1024) break; counter_check(&c, chunk, n); }
    if (capture_mode == 'c') CHECK(c.gaps == gaps_before + 1 && c.jump > 14400 - 4096, "after recover: gaps %ld jump %ld", c.gaps - gaps_before, c.jump);
    CHECK(can_pause, "hw:0,0 capture should support pause");
    if (can_pause) {
        CHECK(snd_pcm_pause(pcm, 1) == 0 && snd_pcm_state(pcm) == STATE_PAUSED, "pause");
        sleep_ms(200);
        CHECK(snd_pcm_pause(pcm, 0) == 0 && snd_pcm_state(pcm) == STATE_RUNNING, "resume");
        long gaps = c.gaps;
        for (int i = 0; i < 3; i++) { n = snd_pcm_readi(pcm, chunk, 1024); CHECK(n == 1024, "read after resume %ld", n); if (n != 1024) break; counter_check(&c, chunk, n); }
        if (capture_mode == 'c') CHECK(c.gaps == gaps + 1 && c.jump > 4800 && c.jump < 48000, "pause: gaps %ld, skipped %ld frames", c.gaps - gaps, c.jump);
    }
    sleep_ms(30);
    CHECK(snd_pcm_drain(pcm) == 0, "capture drain");
    long drained = 0; while ((n = snd_pcm_readi(pcm, chunk, 1024)) > 0) drained += n;
    printf("[64] AUDIO CD xrun_jump=%ld drained=%ld final=%ld state=%d\n", c.jump, drained, n, snd_pcm_state(pcm));
    CHECK(drained > 0 && (n == -EPIPE || n == -EBADFD || n == 0) && snd_pcm_state(pcm) == STATE_SETUP, "drain kept %ld frames, then %ld state %d", drained, n, snd_pcm_state(pcm));
    snd_pcm_close(pcm);
}
/* CE: two recorders at once (two substreams): both continuous, both on the same device timeline. */
static void capture_two_clients(void) {
    snd_pcm_t* a = open_capture_hw(0, RW_INTERLEAVED, 960, 9600, NULL); snd_pcm_t* b = open_capture_hw(0, RW_INTERLEAVED, 960, 9600, NULL);
    if (!a || !b) { if (a) snd_pcm_close(a); if (b) snd_pcm_close(b); return; }
    int16_t chunk[960 * 2]; struct counter ca = {0}, cb = {0}; long skew = 0;
    for (int i = 0; i < 50; i++) {
        snd_pcm_sframes_t n = snd_pcm_readi(a, chunk, 960); CHECK(n == 960, "client A read %ld", n); if (n != 960) break; counter_check(&ca, chunk, n);
        n = snd_pcm_readi(b, chunk, 960); CHECK(n == 960, "client B read %ld", n); if (n != 960) break; counter_check(&cb, chunk, n);
        long s = labs((long)(int32_t)(ca.next - cb.next)); if (s > skew) skew = s;
    }
    printf("[64] AUDIO CE frames=%ld/%ld gaps=%ld/%ld skew=%ld\n", ca.frames, cb.frames, ca.gaps, cb.gaps, skew);
    if (capture_mode == 'c') CHECK(!ca.gaps && !cb.gaps && skew < 9600, "two clients: gaps %ld/%ld, skew %ld", ca.gaps, cb.gaps, skew);
    snd_pcm_close(a); snd_pcm_close(b);
}
/* CF: full duplex: a 440 Hz tone played through "default" while "default" records; no xrun on either side. */
static void capture_duplex(void) {
    snd_pcm_t* out = open_pcm("default", 0, RW_INTERLEAVED, 48000, 200000); snd_pcm_t* in = open_capture("default", 0, 2, 48000, 100000);
    if (!out || !in) { if (out) snd_pcm_close(out); if (in) snd_pcm_close(in); return; }
    int16_t block[480 * 2]; double phase = 0; int xruns_out = 0, xruns_in = 0; struct counter c = {0}; long recorded = 0, played = 0;
    CHECK(snd_pcm_start(in) == 0, "duplex capture start");
    for (int i = 0; i < 10; i++) { tone(block, 480, &phase, 440, 48000); if (snd_pcm_writei(out, block, 480) == 480) played += 480; }
    double start = now_s();
    while (now_s() - start < 2.0) {
        snd_pcm_sframes_t n = snd_pcm_readi(in, block, 480);
        if (n == -EPIPE) { xruns_in++; snd_pcm_recover(in, (int)n, 1); snd_pcm_start(in); continue; }
        if (n > 0) { recorded += n; if (capture_mode == 'c') counter_check(&c, block, n); }
        tone(block, 480, &phase, 440, 48000);
        n = snd_pcm_writei(out, block, 480);
        if (n == -EPIPE) { xruns_out++; snd_pcm_recover(out, (int)n, 1); continue; }
        if (n > 0) played += n;
    }
    printf("[64] AUDIO CF recorded=%ld played=%ld xruns_in=%d xruns_out=%d gaps=%ld\n", recorded, played, xruns_in, xruns_out, c.gaps);
    CHECK(!xruns_in && !xruns_out && recorded > 48000 * 1.9 && played > 48000 * 1.9, "full duplex: in %ld out %ld xruns %d/%d", recorded, played, xruns_in, xruns_out);
    if (capture_mode == 'c') CHECK(!c.gaps, "duplex counter gaps %ld", c.gaps);
    snd_pcm_drop(out); snd_pcm_close(out); snd_pcm_close(in);
}
/* CG: the capture mixer through alsa-lib's simple mixer (CTL ELEM_LIST/INFO/READ/WRITE/TLV): "Capture" volume with a dB
   range and a switch; values written read back. */
static void capture_mixer(void) {
    snd_mixer_t* mixer = NULL; int error = snd_mixer_open(&mixer, 0);
    if (!error) error = snd_mixer_attach(mixer, "hw:0");
    if (!error) error = snd_mixer_selem_register(mixer, NULL, NULL);
    if (!error) error = snd_mixer_load(mixer);
    CHECK(error == 0, "mixer: %s", snd_strerror(error));
    if (error) { if (mixer) snd_mixer_close(mixer); return; }
    snd_mixer_elem_t* capture = NULL; int elements = 0;
    for (snd_mixer_elem_t* e = snd_mixer_first_elem(mixer); e; e = snd_mixer_elem_next(e)) {
        elements++; printf("[64] AUDIO mixer element \"%s\" capture_volume=%d capture_switch=%d\n", snd_mixer_selem_get_name(e),
                           snd_mixer_selem_has_capture_volume(e), snd_mixer_selem_has_capture_switch(e));
        if (!strcmp(snd_mixer_selem_get_name(e), "Capture")) capture = e;
    }
    CHECK(capture && snd_mixer_selem_has_capture_volume(capture) && snd_mixer_selem_has_capture_switch(capture), "Capture volume+switch element");
    if (capture) {
        long min = 0, max = 0, db_min = 0, db_max = 0, value = -1, db = 0; int on = -1;
        CHECK(snd_mixer_selem_get_capture_volume_range(capture, &min, &max) == 0 && max > min, "volume range %ld..%ld", min, max);
        CHECK(snd_mixer_selem_get_capture_dB_range(capture, &db_min, &db_max) == 0 && db_max > db_min, "dB range %ld..%ld", db_min, db_max);
        long saved = 0; snd_mixer_selem_get_capture_volume(capture, 0, &saved);
        CHECK(snd_mixer_selem_set_capture_volume_all(capture, min) == 0 && snd_mixer_selem_get_capture_volume(capture, 1, &value) == 0 && value == min, "set min -> %ld", value);
        CHECK(snd_mixer_selem_get_capture_dB(capture, 0, &db) == 0 && db == db_min, "dB at min %ld (range %ld)", db, db_min);
        CHECK(snd_mixer_selem_set_capture_volume_all(capture, saved) == 0, "restore volume");
        CHECK(snd_mixer_selem_set_capture_switch_all(capture, 0) == 0 && snd_mixer_selem_get_capture_switch(capture, 0, &on) == 0 && on == 0, "switch off -> %d", on);
        CHECK(snd_mixer_selem_set_capture_switch_all(capture, 1) == 0, "switch on");
        printf("[64] AUDIO CG elements=%d capture=%ld..%ld dB=%ld..%ld saved=%ld\n", elements, min, max, db_min, db_max, saved);
    }
    snd_mixer_close(mixer);
}
/* CH: Debian's arecord (alsa-utils 1.2.14 aplay binary, installed as /bin/arecord by tests/test_x64_capture.py): the
   device list, a 2 s WAV through "default", an mmap raw capture on hw:0,0, a 16 kHz mono capture through plughw, and
   clean failures for a missing card and an unsupported channel count. Output goes to a pipe; a 20 s deadline kills a
   hung recorder. */
static char arecord_errors[2048];
static bool arecord_expect_failure;   /* the deliberate failures below: their stderr is checked, not logged */
static long run_arecord(const char* const* args, uint8_t* out, long room, int* status) {
    int data[2], errors[2]; arecord_errors[0] = 0;
    if (pipe(data)) { CHECK(false, "pipe: %s", strerror(errno)); return -1; }
    if (pipe(errors)) { CHECK(false, "pipe: %s", strerror(errno)); close(data[0]); close(data[1]); return -1; }
    pid_t child = fork();
    if (child < 0) { CHECK(false, "fork: %s", strerror(errno)); close(data[0]); close(data[1]); close(errors[0]); close(errors[1]); return -1; }
    if (!child) {
        dup2(data[1], 1); dup2(errors[1], 2); close(data[0]); close(data[1]); close(errors[0]); close(errors[1]);
        execv("/bin/arecord", (char* const*)args); _exit(127);
    }
    close(data[1]); close(errors[1]);
    long used = 0, error_used = 0; double deadline = now_s() + 20; bool killed = false, data_open = true, errors_open = true;
    while (data_open || errors_open) {
        struct pollfd fds[2] = {{data_open ? data[0] : -1, POLLIN, 0}, {errors_open ? errors[0] : -1, POLLIN, 0}};
        if (poll(fds, 2, 200) > 0) {
            if (fds[0].revents) {
                uint8_t scratch[4096]; bool keep = used < room;
                ssize_t n = read(data[0], keep ? out + used : scratch, keep ? (size_t)(room - used < 65536 ? room - used : 65536) : sizeof(scratch));
                if (n <= 0) data_open = false; else if (keep) used += n;
            }
            if (fds[1].revents) {
                char scratch[512]; ssize_t n = read(errors[0], scratch, sizeof(scratch));
                if (n <= 0) errors_open = false;
                else for (ssize_t i = 0; i < n && error_used + 1 < (long)sizeof(arecord_errors); i++) arecord_errors[error_used++] = scratch[i];
                arecord_errors[error_used] = 0;
            }
        }
        if (now_s() > deadline && !killed) { kill(child, SIGKILL); killed = true; }
    }
    close(data[0]); close(errors[0]);
    if (waitpid(child, status, 0) != child) *status = -1;
    CHECK(!killed, "arecord %s did not finish within 20 s", args[1]);
    bool ok = WIFEXITED(*status) && !WEXITSTATUS(*status);
    for (char* c = arecord_errors; *c; c++) if (*c == '\n') *c = ' ';      /* one log line */
    if (!ok && arecord_errors[0] && !arecord_expect_failure) printf("[64] AUDIO CH arecord %s exit %d: %.300s\n", args[1], WIFEXITED(*status) ? WEXITSTATUS(*status) : -1, arecord_errors);
    return used;
}
static uint32_t le32_at(const uint8_t* p) { return p[0] | (uint32_t)p[1] << 8 | (uint32_t)p[2] << 16 | (uint32_t)p[3] << 24; }
/* Content of S16 frames: counter continuity, tone frequency, or silence (per capture mode). */
static void check_recording(const char* what, const int16_t* data, long frames, unsigned channels, unsigned rate, bool exact) {
    struct counter c = {0}; long crossings = 0; int peak = 0; int16_t last = 0;
    for (long i = 0; i < frames; i++) {
        int16_t v = data[i * channels]; if ((last < 0) != (v < 0)) crossings++; last = v;
        for (unsigned k = 0; k < channels; k++) { int a = abs(data[i * channels + k]); if (a > peak) peak = a; }
    }
    if (channels == 2) counter_check(&c, data, frames);
    double frequency = frames ? crossings / 2.0 / ((double)frames / rate) : 0;
    printf("[64] AUDIO CH %s frames=%ld gaps=%ld peak=%d frequency=%.1f\n", what, frames, c.gaps, peak, frequency);
    if (capture_mode == 'c' && exact) CHECK(c.gaps == 0 && frames > 0, "%s: counter gaps %ld (jump %ld)", what, c.gaps, c.jump);
    if (capture_mode == 'k') CHECK(fabs(frequency - 1000) < 20 && peak >= 7000 && peak <= 8192 + 64, "%s: tone %.1f Hz peak %d", what, frequency, peak);
    if (capture_mode == 'z') CHECK(peak == 0, "%s: silence expected, peak %d", what, peak);
}
static void capture_arecord(void) {
    if (access("/bin/arecord", X_OK)) { CHECK(false, "/bin/arecord is not installed"); return; }
    static uint8_t buffer[1 << 20]; int status = -1;
    const char* list[] = {"arecord", "-l", NULL};
    long n = run_arecord(list, buffer, sizeof(buffer) - 1, &status); buffer[n > 0 ? n : 0] = 0;
    printf("[64] AUDIO CH arecord -l status=%d: %s", status, (char*)buffer);
    CHECK(WIFEXITED(status) && !WEXITSTATUS(status) && strstr((char*)buffer, "card 0:") && strstr((char*)buffer, "device 0:"), "arecord -l");
    /* 2 s WAV through the default (plug) device */
    const char* wav[] = {"arecord", "-q", "-D", "default", "-f", "S16_LE", "-c", "2", "-r", "48000", "-d", "2", "-t", "wav", "-", NULL};
    n = run_arecord(wav, buffer, sizeof(buffer), &status);
    bool header = n >= 44 && !memcmp(buffer, "RIFF", 4) && !memcmp(buffer + 8, "WAVEfmt ", 8) && buffer[20] == 1 && buffer[22] == 2 &&
                  le32_at(buffer + 24) == 48000 && buffer[34] == 16 && !memcmp(buffer + 36, "data", 4);
    long data_bytes = header ? (long)le32_at(buffer + 40) : 0;
    printf("[64] AUDIO CH arecord wav status=%d bytes=%ld data=%ld\n", status, n, data_bytes);
    CHECK(WIFEXITED(status) && !WEXITSTATUS(status) && header && data_bytes == 2 * 48000 * 4 && n == 44 + data_bytes, "arecord 2 s WAV: status %d, %ld bytes, data %ld", status, n, data_bytes);
    if (header && n >= 44 + data_bytes) check_recording("wav", (const int16_t*)(buffer + 44), data_bytes / 4, 2, 48000, true);
    /* mmap access straight on the hardware device, sample count */
    const char* mmap_args[] = {"arecord", "-q", "-D", "hw:0,0", "-M", "-f", "S16_LE", "-c", "2", "-r", "48000", "-s", "24000", "-t", "raw", "-", NULL};
    n = run_arecord(mmap_args, buffer, sizeof(buffer), &status);
    printf("[64] AUDIO CH arecord mmap status=%d bytes=%ld\n", status, n);
    CHECK(WIFEXITED(status) && !WEXITSTATUS(status) && n == 24000 * 4, "arecord -M hw:0,0: status %d, %ld bytes", status, n);
    if (n == 24000 * 4) check_recording("mmap", (const int16_t*)buffer, 24000, 2, 48000, true);
    /* 16 kHz mono through plughw (alsa-lib rate + channel conversion) */
    const char* plug[] = {"arecord", "-q", "-D", "plughw:0,0", "-f", "S16_LE", "-c", "1", "-r", "16000", "-d", "1", "-t", "raw", "-", NULL};
    n = run_arecord(plug, buffer, sizeof(buffer), &status);
    printf("[64] AUDIO CH arecord plughw 16k mono status=%d bytes=%ld\n", status, n);
    CHECK(WIFEXITED(status) && !WEXITSTATUS(status) && n == 16000 * 2, "arecord plughw 16 kHz mono: status %d, %ld bytes", status, n);
    if (n == 16000 * 2) check_recording("plug16k", (const int16_t*)buffer, 16000, 1, 16000, false);
    /* failures: no such card, channel count the hardware does not have */
    const char* missing[] = {"arecord", "-q", "-D", "hw:7,0", "-d", "1", "-t", "raw", "-", NULL};
    arecord_expect_failure = true;
    n = run_arecord(missing, buffer, sizeof(buffer), &status);
    CHECK(WIFEXITED(status) && WEXITSTATUS(status) && n == 0 && strstr(arecord_errors, "audio open error"), "arecord on a missing card must fail: status %d, %ld bytes", status, n);
    const char* channels[] = {"arecord", "-q", "-D", "hw:0,0", "-f", "S16_LE", "-c", "6", "-r", "48000", "-d", "1", "-t", "raw", "-", NULL};
    n = run_arecord(channels, buffer, sizeof(buffer), &status);
    arecord_expect_failure = false;
    printf("[64] AUDIO CH arecord failures: missing card ok, 6 channels status=%d bytes=%ld\n", status, n);
    CHECK(WIFEXITED(status) && WEXITSTATUS(status) && n == 0 && strstr(arecord_errors, "Channels count non available"), "arecord -c 6 on hw:0,0 must fail: status %d, %ld bytes", status, n);
}
static void capture_absent(void) {
    struct stat st;
    CHECK(stat("/dev/snd/pcmC0D0c", &st) == -1 && errno == ENOENT, "pcmC0D0c must not exist without an input path");
    snd_pcm_t* pcm = NULL; int error = snd_pcm_open(&pcm, "hw:0,0", CAPTURE, NONBLOCK);
    printf("[64] AUDIO nocapture open -> %d (%s)\n", error, error < 0 ? snd_strerror(error) : "opened");
    CHECK(error < 0, "capture open must fail without a capture device");
    if (!error) snd_pcm_close(pcm);
}
static void capture_campaign(char mode) {
    capture_mode = mode;
    printf("[64] AUDIO TEST START capture mode=%c\n", mode);
    enumerate();
    if (mode == 'x') capture_absent();
    else {
        capture_enumerate(); capture_blocking(); capture_chrome_pattern(); capture_mmap(); capture_xrun_pause_drain();
        capture_two_clients(); capture_duplex(); capture_mixer(); capture_arecord();
    }
    printf(failures ? "[64] AUDIO TEST FAILED failures=%d\n" : "[64] AUDIO TEST PASSED failures=%d\n", failures);
}
int main(void) {
    setvbuf(stdout, NULL, _IOLBF, 0);
    char mode = 0, option = 0; FILE* config = fopen("/nexus/test-audio", "r");
    if (config) { mode = (char)fgetc(config); option = (char)fgetc(config); fclose(config); }
    /* Second byte 'w' (VMware): start 5 s after boot, since VMware runs its virtual time fast for a few seconds after
       a boot that kept interrupts off (the kernel's self-tests); and judge phase A's rate by elapsed time, since its
       HD Audio DMA position advances in ~50 ms steps that make a 50 ms-sampled fit noisy. */
    if (option == 'w') { coarse_position = true; struct timespec pause = {5, 0}; nanosleep(&pause, NULL); }
    if (mode == 'l' || mode == 's') {
        if (mode == 'l') probe_latency(); else probe_stability();
        printf(failures ? "[64] AUDIO TEST FAILED\n" : "[64] AUDIO TEST PASSED probe\n"); return failures != 0;
    }
    if (mode == 'c' || mode == 'k' || mode == 'z' || mode == 'x') { capture_campaign(mode); return failures ? 1 : 0; }
    if (mode == 'n' || mode == 'q') {
        printf("[64] AUDIO TEST START mode=%c\n", mode);
        if (mode == 'n') no_card(); else { enumerate(); quick(); }
        printf(failures ? "[64] AUDIO TEST FAILED failures=%d\n" : "[64] AUDIO TEST PASSED failures=%d\n", failures);
        return failures ? 1 : 0;
    }
    printf("[64] AUDIO TEST START\n");
    enumerate(); phase_a(); phase_b(); phase_c(); phase_d(); phase_e(); phase_f();
    printf(failures ? "[64] AUDIO TEST FAILED failures=%d\n" : "[64] AUDIO TEST PASSED failures=%d\n", failures);
    return failures ? 1 : 0;
}
