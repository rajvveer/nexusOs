/* Tickless idle campaign program (/bin/power64 for tests/test_x64_tickless.py; also /bin/tickless64 on apps64.img).
 *
 * The campaign file /nexus/test-power holds "t <modes>" (the kernel's power campaign runs /bin/power64):
 *   m<seconds>  measure an idle system: this program only sleeps; per CPU the cpuidle entries (= wake-ups from idle),
 *               the idle residency and, when the kernel has them, the local-timer / PIT interrupt counts of
 *               /proc/interrupts and the tick-stop statistics of /proc/timer_list. Prints "[64] TICK MEASURE ...".
 *   a           timer accuracy: nanosleep / clock_nanosleep (relative, TIMER_ABSTIME, CLOCK_REALTIME/BOOTTIME), poll,
 *               epoll_wait, select, futex timeouts, ITIMER_REAL periods, alarm(), timerfd and POSIX timers when present,
 *               a wake-up after a long idle period; never early, bounded lateness; CLOCK_MONOTONIC vs REALTIME.
 *   h           hostile inputs: invalid timespecs/clocks/flags, bad pointers, sleeps interrupted by signals with the
 *               remaining time, huge timeouts, zero timeouts, many concurrent sleepers with distinct deadlines.
 *   b           busy CPUs: a thread spins (an application processor or the boot CPU is never idle) while other threads
 *               sleep short periods; their sleeps stay accurate (the tick runs where it is needed).
 *   p<seconds>  audio playback: a 440 Hz tone through libasound (hw:0,0) as media players feed it, measured like m:
 *               blocking snd_pcm_writei with a 500 ms buffer, then non-blocking writes paced by snd_pcm_wait (poll)
 *               with a 100 ms buffer. Prints "[64] TICK MEASURE audio-<phase> ..."; underruns are failures.
 *   P           playback across system sleep: suspend-to-idle then suspend-to-RAM (RTC wake) while a child plays;
 *               the tone switches 440/660/880 Hz after each resume (tests/test_x64_tickless.py checks the capture).
 *   s           sleep then suspend-to-idle interaction is not here (test_x64_suspend.py covers sleep).
 * Every check failure prints "[64] TICK FAIL: ..."; the run ends with "[64] TICK DONE failures=<n>". */
#define _GNU_SOURCE
#include <dlfcn.h>
#include <errno.h>
#include <fcntl.h>
#include <linux/futex.h>
#include <poll.h>
#include <pthread.h>
#include <signal.h>
#include <stdarg.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/epoll.h>
#include <sys/select.h>
#include <sys/syscall.h>
#include <sys/time.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>

static int failures;
static void fail(const char* fmt, ...) {
    va_list ap; va_start(ap, fmt); printf("[64] TICK FAIL: "); vprintf(fmt, ap); printf("\n"); va_end(ap); fflush(stdout); failures++;
}
static void say(const char* fmt, ...) { va_list ap; va_start(ap, fmt); printf("[64] TICK "); vprintf(fmt, ap); printf("\n"); va_end(ap); fflush(stdout); }
static uint64_t now_ns(clockid_t clock) { struct timespec t; clock_gettime(clock, &t); return (uint64_t)t.tv_sec * 1000000000ULL + (uint64_t)t.tv_nsec; }
static uint64_t mono(void) { return now_ns(CLOCK_MONOTONIC); }
static struct timespec ts(uint64_t ns) { struct timespec t = {(time_t)(ns / 1000000000ULL), (long)(ns % 1000000000ULL)}; return t; }
static char* read_file(const char* path, char* out, size_t size) {
    int fd = open(path, O_RDONLY); out[0] = 0; if (fd < 0) return NULL;
    size_t used = 0; ssize_t n;
    while (used + 1 < size && (n = read(fd, out + used, size - 1 - used)) > 0) used += (size_t)n;
    close(fd); out[used] = 0; return out;
}
static long long read_number(const char* path) { char text[64]; return read_file(path, text, sizeof(text)) ? atoll(text) : -1; }

/* ---- lateness statistics ---- */
static uint64_t late_limit_ns = 20000000ULL;   /* one sleep may end at most this late (TCG on a loaded host) */
struct stats { const char* name; unsigned n; uint64_t sum, max; uint64_t samples[64]; };
static void stats_add(struct stats* s, uint64_t wanted, uint64_t elapsed) {
    if (elapsed + 50000 < wanted) { fail("%s woke %llu us early (wanted %llu us)", s->name, (unsigned long long)((wanted - elapsed) / 1000), (unsigned long long)(wanted / 1000)); return; }
    uint64_t late = elapsed > wanted ? elapsed - wanted : 0;
    if (late > late_limit_ns) fail("%s woke %llu us late (wanted %llu us)", s->name, (unsigned long long)(late / 1000), (unsigned long long)(wanted / 1000));
    if (s->n < 64) s->samples[s->n] = late;
    s->n++; s->sum += late; if (late > s->max) s->max = late;
}
static int compare(const void* a, const void* b) { uint64_t x = *(const uint64_t*)a, y = *(const uint64_t*)b; return x < y ? -1 : x > y; }
static void stats_report(struct stats* s) {
    if (!s->n) return;
    unsigned k = s->n < 64 ? s->n : 64; qsort(s->samples, k, sizeof(s->samples[0]), compare);
    say("LATENCY %s n=%u median_us=%llu mean_us=%llu max_us=%llu", s->name, s->n, (unsigned long long)(s->samples[k / 2] / 1000),
        (unsigned long long)(s->sum / s->n / 1000), (unsigned long long)(s->max / 1000));
    if (s->samples[k / 2] > 5000000ULL) fail("%s median lateness %llu us", s->name, (unsigned long long)(s->samples[k / 2] / 1000));
}

/* ---- idle measurement ---- */
#define CPUS 32
struct idle_counters { unsigned cpus; long long usage[CPUS], time_us[CPUS], local[CPUS], pit[CPUS], resched[CPUS]; bool have_irq; char timer_list[8192]; };
static unsigned online_cpus(void) { long n = sysconf(_SC_NPROCESSORS_ONLN); return n < 1 ? 1 : n > CPUS ? CPUS : (unsigned)n; }
static void parse_interrupts(struct idle_counters* c) {
    static char text[16384]; c->have_irq = read_file("/proc/interrupts", text, sizeof(text)) != NULL && text[0];
    if (!c->have_irq) return;
    for (char* line = strtok(text, "\n"); line; line = strtok(NULL, "\n")) {
        while (*line == ' ') line++;
        long long* row = !strncmp(line, "LOC:", 4) ? c->local : !strncmp(line, "0:", 2) ? c->pit : !strncmp(line, "RES:", 4) ? c->resched : NULL;
        if (!row) continue;
        char* p = strchr(line, ':') + 1;
        for (unsigned i = 0; i < c->cpus; i++) { while (*p == ' ') p++; row[i] = strtoll(p, &p, 10); }
    }
}
static void sample(struct idle_counters* c) {
    memset(c, 0, sizeof(*c)); c->cpus = online_cpus();
    for (unsigned cpu = 0; cpu < c->cpus; cpu++)
        for (unsigned state = 0; state < 8; state++) {
            char path[128]; snprintf(path, sizeof(path), "/sys/devices/system/cpu/cpu%u/cpuidle/state%u/usage", cpu, state);
            long long usage = read_number(path); if (usage < 0) break;
            snprintf(path, sizeof(path), "/sys/devices/system/cpu/cpu%u/cpuidle/state%u/time", cpu, state);
            c->usage[cpu] += usage; c->time_us[cpu] += read_number(path);
        }
    parse_interrupts(c);
    read_file("/proc/timer_list", c->timer_list, sizeof(c->timer_list));
}
/* label "": the idle measurement (only this sleeps); otherwise work(seconds) runs meanwhile ("MEASURE <label> cpu..."). */
static void measure_with(const char* label, unsigned seconds, void (*work)(unsigned seconds)) {
    struct idle_counters before, after;
    const char* gap = label[0] ? " " : "";
    sample(&before); say("MEASURE%s%s BEGIN %u s", gap, label, seconds); uint64_t start = mono();
    if (work) work(seconds);
    else { struct timespec t = ts((uint64_t)seconds * 1000000000ULL); while (nanosleep(&t, &t) && errno == EINTR) {} }
    uint64_t elapsed = mono() - start; sample(&after); say("MEASURE%s%s END", gap, label);
    double s = (double)elapsed / 1e9; double total_wakeups = 0, total_local = 0;
    for (unsigned cpu = 0; cpu < after.cpus; cpu++) {
        double wakeups = (double)(after.usage[cpu] - before.usage[cpu]) / s;
        double residency = (double)(after.time_us[cpu] - before.time_us[cpu]) / 1e6 / s * 100.0;
        total_wakeups += wakeups;
        if (after.have_irq) {
            double local = (double)(after.local[cpu] - before.local[cpu]) / s, pit = (double)(after.pit[cpu] - before.pit[cpu]) / s;
            double res = (double)(after.resched[cpu] - before.resched[cpu]) / s; total_local += local + pit;
            say("MEASURE%s%s cpu%u wakeups_per_s=%.1f idle_residency=%.1f%% timer_irqs_per_s=%.1f local_per_s=%.1f pit_per_s=%.1f resched_per_s=%.1f",
                gap, label, cpu, wakeups, residency, local + pit, local, pit, res);
        } else say("MEASURE%s%s cpu%u wakeups_per_s=%.1f idle_residency=%.1f%% timer_irqs_per_s=n/a", gap, label, cpu, wakeups, residency);
    }
    say("MEASURE%s%s total cpus=%u seconds=%.2f wakeups_per_s=%.1f timer_irqs_per_s=%s%.1f", gap, label, after.cpus, s, total_wakeups,
        after.have_irq ? "" : "n/a ", after.have_irq ? total_local : 0.0);
    if (after.timer_list[0] && !label[0]) {
        for (char* line = strtok(after.timer_list, "\n"); line; line = strtok(NULL, "\n"))
            if (strstr(line, "cpu:") || strstr(line, "nohz_mode") || strstr(line, "idle_calls") || strstr(line, "idle_sleeps") ||
                strstr(line, "idle_sleeptime") || strstr(line, "tick_stopped") || strstr(line, "Clock Event Device") || strstr(line, "event_handler") ||
                strstr(line, "wakeups") || strstr(line, "early_events") || strstr(line, "event_late") || strstr(line, "event_advance") || strstr(line, "waited_out"))
                say("TIMER_LIST %s", line);
    }
}
static void measure(unsigned seconds) { measure_with("", seconds, NULL); }

/* ---- audio playback (mode p) ---- */
/* libasound through dlopen (no link-time dependency; the test installs Debian's libasound.so.2 and /usr/share/alsa). */
typedef struct _snd_pcm snd_pcm_t;
static struct {
    int (*open)(snd_pcm_t**, const char*, int, int);
    int (*set_params)(snd_pcm_t*, int, int, unsigned, unsigned, int, unsigned);
    int (*get_params)(snd_pcm_t*, unsigned long*, unsigned long*);
    long (*writei)(snd_pcm_t*, const void*, unsigned long);
    long (*avail_update)(snd_pcm_t*);
    int (*wait)(snd_pcm_t*, int);
    int (*recover)(snd_pcm_t*, int, int);
    int (*drain)(snd_pcm_t*);
    int (*close)(snd_pcm_t*);
    const char* (*strerror)(int);
} alsa;
static bool alsa_load(void) {
    void* lib = dlopen("libasound.so.2", RTLD_NOW);
    if (!lib) { fail("audio: dlopen libasound.so.2: %s", dlerror()); return false; }
    void** slots = (void**)&alsa;
    static const char* const names[] = {"snd_pcm_open", "snd_pcm_set_params", "snd_pcm_get_params", "snd_pcm_writei", "snd_pcm_avail_update",
                                        "snd_pcm_wait", "snd_pcm_recover", "snd_pcm_drain", "snd_pcm_close", "snd_strerror"};
    for (unsigned i = 0; i < sizeof(names) / sizeof(names[0]); i++)
        if (!(slots[i] = dlsym(lib, names[i]))) { fail("audio: %s missing", names[i]); return false; }
    return true;
}
static int16_t tone_block[48000 * 2];       /* 1 s of a whole-Hz tone at 48 kHz: loops seamlessly */
static unsigned tone_at;
/* hz at quarter amplitude by the symplectic sine recurrence (no libm): k = 2 sin(w/2) makes its frequency exact. */
static void tone_make(unsigned hz) {
    const double x = 3.14159265358979323846 * hz / 48000, k = 2 * (x - x * x * x / 6 + x * x * x * x * x / 120 - x * x * x * x * x * x * x / 5040);
    double s = 0, c = 1;
    for (unsigned i = 0; i < 48000; i++) { int16_t v = (int16_t)(s * 8000); tone_block[2 * i] = tone_block[2 * i + 1] = v; s += k * c; c -= k * s; }
    tone_at = 0;
}
static const int16_t* tone_next(unsigned long frames) {
    static int16_t chunk[48000 * 2];
    for (unsigned long i = 0; i < frames && i < 48000; i++) { chunk[2 * i] = tone_block[2 * tone_at]; chunk[2 * i + 1] = tone_block[2 * tone_at + 1]; tone_at = (tone_at + 1) % 48000; }
    return chunk;
}
static bool audio_blocking;     /* phase: blocking writei with a 500 ms buffer, else snd_pcm_wait-paced with 100 ms */
static void audio_play(unsigned seconds) {
    snd_pcm_t* pcm = NULL; const char* how = audio_blocking ? "blocking" : "poll";
    int error = alsa.open(&pcm, "hw:0,0", 0, audio_blocking ? 0 : 1);
    if (error < 0) { fail("audio %s: open hw:0,0: %s", how, alsa.strerror(error)); return; }
    error = alsa.set_params(pcm, 2, 3, 2, 48000, 1, audio_blocking ? 500000 : 100000);   /* S16_LE, RW interleaved, stereo */
    if (error < 0) { fail("audio %s: set_params: %s", how, alsa.strerror(error)); alsa.close(pcm); return; }
    unsigned long buffer = 0, period = 0; alsa.get_params(pcm, &buffer, &period);
    unsigned long total = (unsigned long)seconds * 48000, written = 0; unsigned xruns = 0, waits = 0;
    while (written < total) {
        unsigned long want = total - written < period ? total - written : period;
        if (!audio_blocking) {
            long room = alsa.avail_update(pcm);
            if (room < 0) { xruns++; alsa.recover(pcm, (int)room, 1); continue; }
            if ((unsigned long)room < want) { waits++; alsa.wait(pcm, 1000); continue; }
            want = (unsigned long)room < total - written ? (unsigned long)room : total - written;
            if (want > 48000) want = 48000;
        }
        long n = alsa.writei(pcm, tone_next(want), want);
        if (n == -32) { xruns++; alsa.recover(pcm, (int)n, 1); continue; }
        if (n == -11) { waits++; alsa.wait(pcm, 1000); continue; }
        if (n < 0) { fail("audio %s: writei: %s", how, alsa.strerror((int)n)); break; }
        written += (unsigned long)n;
        if ((unsigned long)n < want) tone_at = (unsigned)((tone_at + 48000 - (want - (unsigned long)n)) % 48000);   /* replay the unwritten part */
    }
    alsa.drain(pcm); alsa.close(pcm);
    say("AUDIO %s buffer=%lu period=%lu frames=%lu xruns=%u waits=%u", how, buffer, period, written, xruns, waits);
    if (xruns) fail("audio %s: %u underruns", how, xruns);
}
static void audio(unsigned seconds) {
    if (!alsa_load()) return;
    tone_make(440);
    audio_blocking = true; measure_with("audio-blocking", seconds, audio_play);
    audio_blocking = false; measure_with("audio-poll", seconds, audio_play);
}
/* P: system sleep in the middle of playback. A child plays with blocking writes (500 ms buffer) 440 Hz, then 660 Hz
 * after the parent's first SIGUSR1 and 880 Hz after the second; the parent lets it play 2.5 s, suspends to idle
 * ("freeze") with an RTC wake alarm 2 s ahead, signals, lets it play 2.5 s, suspends to RAM ("mem"), signals. The
 * child plays 3 s after the second signal, drains and reports; an xrun per sleep (the queued audio is skipped, as on
 * Linux, where snd_pcm_recover handles the suspend) is allowed, a stuck or failing stream is not. */
static volatile sig_atomic_t tone_switches;
static void on_tone_switch(int s) { (void)s; tone_switches++; }
static void audio_sleep_child(void) {
    snd_pcm_t* pcm = NULL; int error = alsa.open(&pcm, "hw:0,0", 0, 0);
    if (error < 0) { fail("audio sleep: open hw:0,0: %s", alsa.strerror(error)); _exit(1); }
    if ((error = alsa.set_params(pcm, 2, 3, 2, 48000, 1, 500000)) < 0) { fail("audio sleep: set_params: %s", alsa.strerror(error)); _exit(1); }
    unsigned long buffer = 0, period = 0, written = 0; alsa.get_params(pcm, &buffer, &period);
    unsigned seen = 0, xruns = 0, interrupted = 0; uint64_t end = 0, give_up = mono() + 120000000000ULL;
    tone_make(440);
    while (mono() < give_up && (!end || mono() < end)) {
        if (tone_switches != (sig_atomic_t)seen) {
            seen = (unsigned)tone_switches; tone_make(seen == 1 ? 660 : 880);
            if (seen >= 2 && !end) end = mono() + 3000000000ULL;
        }
        long n = alsa.writei(pcm, tone_next(period), period);
        if (n == -4) { interrupted++; continue; }                       /* EINTR: the switch signal */
        if (n == -32 || n == -86) { xruns++; if ((error = alsa.recover(pcm, (int)n, 1)) < 0) { fail("audio sleep: recover: %s", alsa.strerror(error)); break; } continue; }
        if (n < 0) { fail("audio sleep: writei: %s", alsa.strerror((int)n)); break; }
        written += (unsigned long)n;
    }
    if (!end) fail("audio sleep: only %u tone switches", seen);
    alsa.drain(pcm); alsa.close(pcm);
    say("AUDIO sleep buffer=%lu period=%lu frames=%lu xruns=%u interrupted=%u switches=%u", buffer, period, written, xruns, interrupted, seen);
    if (xruns > 2) fail("audio sleep: %u underruns", xruns);
    fflush(stdout); _exit(failures ? 1 : 0);
}
static void sleep_ms_(unsigned ms) { struct timespec t = ts((uint64_t)ms * 1000000ULL); while (nanosleep(&t, &t) && errno == EINTR) {} }
static void system_sleep(const char* state) {
    int fd = open("/sys/class/rtc/rtc0/wakealarm", O_WRONLY);
    if (fd < 0 || write(fd, "+2", 2) != 2) fail("audio sleep: wakealarm: %s", strerror(errno));
    if (fd >= 0) close(fd);
    uint64_t start = mono(); fd = open("/sys/power/state", O_WRONLY);
    ssize_t n = fd < 0 ? -1 : write(fd, state, strlen(state)); int error = n < 0 ? errno : 0;
    if (fd >= 0) close(fd);
    say("AUDIO system sleep %s: %s after %llu ms", state, error ? strerror(error) : "resumed", (unsigned long long)((mono() - start) / 1000000));
    if (error) fail("audio sleep: write %s to /sys/power/state: %s", state, strerror(error));
}
static void audio_sleep(void) {
    if (!alsa_load()) return;
    struct sigaction action = {0}; action.sa_handler = on_tone_switch; sigaction(SIGUSR1, &action, NULL);   /* no SA_RESTART */
    fflush(stdout); pid_t child = fork();
    if (child < 0) { fail("audio sleep: fork: %s", strerror(errno)); return; }
    if (!child) audio_sleep_child();
    sleep_ms_(2500); system_sleep("freeze"); sleep_ms_(300); kill(child, SIGUSR1);
    sleep_ms_(2500); system_sleep("mem"); sleep_ms_(300); kill(child, SIGUSR1);
    int status = 0;
    if (waitpid(child, &status, 0) != child) fail("audio sleep: waitpid: %s", strerror(errno));
    else if (!WIFEXITED(status) || WEXITSTATUS(status)) fail("audio sleep: player exited with status 0x%x", status);
    signal(SIGUSR1, SIG_DFL);
}

/* ---- accuracy ---- */
static volatile sig_atomic_t alarms, usr1;
static void on_alarm(int s) { (void)s; alarms++; }
static void on_usr1(int s) { (void)s; usr1++; }
static int futex_wait(int* word, int value, const struct timespec* timeout) { return (int)syscall(SYS_futex, word, FUTEX_WAIT_PRIVATE, value, timeout, NULL, 0); }
static void accuracy(void) {
    static const uint64_t delays[] = {1000000ULL, 2000000ULL, 5000000ULL, 13000000ULL, 50000000ULL, 100000000ULL, 333000000ULL, 1000000000ULL};
    struct stats ns = {.name = "nanosleep"}, abs = {.name = "clock_nanosleep_abs"}, real = {.name = "clock_nanosleep_realtime"}, boot = {.name = "clock_nanosleep_boottime"};
    struct stats pl = {.name = "poll"}, ep = {.name = "epoll_wait"}, sel = {.name = "select"}, fx = {.name = "futex"};
    for (unsigned round = 0; round < 2; round++)
        for (unsigned i = 0; i < sizeof(delays) / sizeof(delays[0]); i++) {
            uint64_t d = delays[i], start = mono(); struct timespec t = ts(d);
            if (nanosleep(&t, NULL)) fail("nanosleep: %s", strerror(errno));
            stats_add(&ns, d, mono() - start);
            uint64_t target = mono() + d; t = ts(target); start = target - d;
            int error = clock_nanosleep(CLOCK_MONOTONIC, TIMER_ABSTIME, &t, NULL);
            if (error) fail("clock_nanosleep abstime: %s", strerror(error));
            stats_add(&abs, d, mono() - start);
            if (d <= 100000000ULL) {
                uint64_t rtarget = now_ns(CLOCK_REALTIME) + d; t = ts(rtarget); uint64_t rstart = rtarget - d;
                error = clock_nanosleep(CLOCK_REALTIME, TIMER_ABSTIME, &t, NULL);
                if (error) fail("clock_nanosleep realtime: %s", strerror(error));
                stats_add(&real, d, now_ns(CLOCK_REALTIME) - rstart);
                start = now_ns(CLOCK_BOOTTIME); t = ts(d);
                error = clock_nanosleep(CLOCK_BOOTTIME, 0, &t, NULL);
                if (error) fail("clock_nanosleep boottime: %s", strerror(error));
                stats_add(&boot, d, now_ns(CLOCK_BOOTTIME) - start);
            }
            if (d >= 1000000ULL && d <= 333000000ULL && d % 1000000ULL == 0) {
                start = mono(); if (poll(NULL, 0, (int)(d / 1000000ULL))) fail("poll returned early"); stats_add(&pl, d, mono() - start);
                int e = epoll_create1(EPOLL_CLOEXEC); struct epoll_event ev;
                start = mono(); if (epoll_wait(e, &ev, 1, (int)(d / 1000000ULL))) fail("epoll_wait returned early"); stats_add(&ep, d, mono() - start); close(e);
                struct timeval tv = {(time_t)(d / 1000000000ULL), (suseconds_t)(d % 1000000000ULL / 1000)};
                start = mono(); if (select(0, NULL, NULL, NULL, &tv)) fail("select returned early"); stats_add(&sel, d, mono() - start);
                int word = 0; t = ts(d); start = mono();
                if (futex_wait(&word, 0, &t) != -1 || errno != ETIMEDOUT) fail("futex wait: %s", strerror(errno));
                stats_add(&fx, d, mono() - start);
            }
        }
    stats_report(&ns); stats_report(&abs); stats_report(&real); stats_report(&boot); stats_report(&pl); stats_report(&ep); stats_report(&sel); stats_report(&fx);

    /* ITIMER_REAL every 20 ms: 25 periods in phase (no drift: expiry n at start + 20 ms * n). */
    signal(SIGALRM, on_alarm); alarms = 0;
    struct itimerval it = {{0, 20000}, {0, 20000}}; uint64_t start = mono();
    setitimer(ITIMER_REAL, &it, NULL);
    while (alarms < 25) { if (mono() - start > 3000000000ULL) { fail("ITIMER_REAL: %d of 25 periods in 3 s", (int)alarms); break; } pause(); }
    uint64_t elapsed = mono() - start; struct itimerval off = {{0, 0}, {0, 0}}; setitimer(ITIMER_REAL, &off, NULL);
    say("ITIMER_REAL 25 periods of 20 ms in %llu us", (unsigned long long)(elapsed / 1000));
    if (elapsed < 500000000ULL - 1000000ULL || elapsed > 500000000ULL + 30000000ULL) fail("ITIMER_REAL periods took %llu us (500000 expected)", (unsigned long long)(elapsed / 1000));
    alarms = 0; start = mono(); alarm(1);
    while (!alarms && mono() - start < 3000000000ULL) pause();
    elapsed = mono() - start; say("alarm(1) after %llu us", (unsigned long long)(elapsed / 1000));
    if (!alarms || elapsed < 999000000ULL || elapsed > 1000000000ULL + late_limit_ns) fail("alarm(1) after %llu us", (unsigned long long)(elapsed / 1000));

    /* timerfd and POSIX timers (when the kernel has them) */
    int tfd = (int)syscall(SYS_timerfd_create, CLOCK_MONOTONIC, 0);
    if (tfd < 0 && errno == ENOSYS) say("timerfd: ENOSYS (not in this kernel), skipped");
    else if (tfd < 0) fail("timerfd_create: %s", strerror(errno));
    else {
        struct itimerspec spec = {{0, 10000000}, {0, 10000000}}; struct stats tf = {.name = "timerfd"};
        start = mono();   /* before arming: the expiries count from the settime call */
        if (syscall(SYS_timerfd_settime, tfd, 0, &spec, NULL)) fail("timerfd_settime: %s", strerror(errno));
        uint64_t total = 0;
        while (total < 10) { uint64_t n; if (read(tfd, &n, 8) != 8) { fail("timerfd read: %s", strerror(errno)); break; } total += n; }
        stats_add(&tf, total * 10000000ULL, mono() - start); stats_report(&tf); close(tfd);
    }
    timer_t timer; struct sigevent event = {.sigev_notify = SIGEV_SIGNAL, .sigev_signo = SIGUSR1};
    signal(SIGUSR1, on_usr1);
    if (timer_create(CLOCK_MONOTONIC, &event, &timer)) {
        if (errno == ENOSYS) say("timer_create: ENOSYS (not in this kernel), skipped"); else fail("timer_create: %s", strerror(errno));
    } else {
        struct itimerspec spec = {{0, 0}, {0, 30000000}}; usr1 = 0; start = mono();
        if (timer_settime(timer, 0, &spec, NULL)) fail("timer_settime: %s", strerror(errno));
        while (!usr1 && mono() - start < 2000000000ULL) pause();
        struct stats pt = {.name = "posix_timer"}; stats_add(&pt, 30000000ULL, mono() - start); stats_report(&pt);
        if (!usr1) fail("POSIX timer never fired");
        timer_delete(timer);
    }

    /* a wake-up after a long idle period (the tick was stopped meanwhile) */
    struct stats longsleep = {.name = "long_idle_wake"};
    for (unsigned i = 0; i < 3; i++) { struct timespec t = ts(2500000000ULL + i * 7000000ULL); start = mono(); nanosleep(&t, NULL); stats_add(&longsleep, 2500000000ULL + i * 7000000ULL, mono() - start); }
    stats_report(&longsleep);
    /* CLOCK_REALTIME keeps pace with CLOCK_MONOTONIC across idle periods */
    uint64_t m0 = mono(), r0 = now_ns(CLOCK_REALTIME); struct timespec t = ts(1500000000ULL); nanosleep(&t, NULL);
    uint64_t m1 = mono(), r1 = now_ns(CLOCK_REALTIME);
    int64_t drift = (int64_t)(r1 - r0) - (int64_t)(m1 - m0);
    say("REALTIME-MONOTONIC drift over %llu ms idle: %lld us", (unsigned long long)((m1 - m0) / 1000000), (long long)(drift / 1000));
    if (drift > 2000000 || drift < -2000000) fail("CLOCK_REALTIME drifted %lld us from CLOCK_MONOTONIC", (long long)(drift / 1000));
}

/* ---- hostile inputs ---- */
struct sleeper { uint64_t delay, elapsed; };
static void* sleeper_main(void* argument) {
    struct sleeper* s = argument; struct timespec t = ts(s->delay); uint64_t start = mono();
    while (nanosleep(&t, &t) && errno == EINTR) {}
    s->elapsed = mono() - start; return NULL;
}
static void hostile(void) {
    struct timespec bad = {0, 1000000000L};
    if (nanosleep(&bad, NULL) != -1 || errno != EINVAL) fail("nanosleep tv_nsec=1e9 accepted");
    bad.tv_nsec = -1; if (nanosleep(&bad, NULL) != -1 || errno != EINVAL) fail("nanosleep tv_nsec=-1 accepted");
    bad.tv_sec = -1; bad.tv_nsec = 0; if (nanosleep(&bad, NULL) != -1 || errno != EINVAL) fail("nanosleep tv_sec=-1 accepted");
    if (syscall(SYS_nanosleep, (void*)16, NULL) != -1 || errno != EFAULT) fail("nanosleep bad pointer: %s", strerror(errno));
    struct timespec zero = {0, 0}; uint64_t start = mono();
    if (nanosleep(&zero, NULL)) fail("nanosleep(0): %s", strerror(errno));
    if (mono() - start > late_limit_ns) fail("nanosleep(0) took %llu us", (unsigned long long)((mono() - start) / 1000));
    struct timespec ok = {0, 1000};
    if (clock_nanosleep((clockid_t)1234, 0, &ok, NULL) != EINVAL) fail("clock_nanosleep invalid clock accepted");
    if (clock_nanosleep(CLOCK_MONOTONIC, 0x10, &ok, NULL) != EINVAL) say("clock_nanosleep unknown flag bit: accepted (Linux ignores unknown flags too)");
    if (clock_nanosleep(CLOCK_THREAD_CPUTIME_ID, 0, &ok, NULL) != EINVAL) fail("clock_nanosleep on CLOCK_THREAD_CPUTIME_ID accepted");
    struct timespec past = {1, 0}; start = mono();   /* an absolute time long gone returns at once */
    if (clock_nanosleep(CLOCK_MONOTONIC, TIMER_ABSTIME, &past, NULL)) fail("clock_nanosleep in the past failed");
    if (mono() - start > late_limit_ns) fail("clock_nanosleep in the past slept %llu us", (unsigned long long)((mono() - start) / 1000));
    if (poll(NULL, 0, 0) != 0) fail("poll(0) did not return 0");
    struct timeval tv0 = {0, 0}; if (select(0, NULL, NULL, NULL, &tv0) != 0) fail("select(0) did not return 0");
    struct timeval tvbad = {0, 2000000}; if (select(0, NULL, NULL, NULL, &tvbad) != -1 || errno != EINVAL) say("select tv_usec=2e6: %s (Linux normalizes)", strerror(errno));

    /* a long sleep interrupted by a signal from a timer: EINTR with the remaining time */
    signal(SIGALRM, on_alarm); alarms = 0; struct itimerval it = {{0, 0}, {0, 300000}};
    setitimer(ITIMER_REAL, &it, NULL);
    struct timespec big = {3600, 0}, left = {0, 0}; start = mono();
    int r = nanosleep(&big, &left); uint64_t elapsed = mono() - start;
    if (r != -1 || errno != EINTR) fail("nanosleep(3600 s) interrupted by SIGALRM: r=%d %s", r, strerror(errno));
    uint64_t remaining = (uint64_t)left.tv_sec * 1000000000ULL + (uint64_t)left.tv_nsec, expected = 3600000000000ULL - elapsed;
    if (remaining > expected + 50000000ULL || remaining + 50000000ULL < expected) fail("remaining %llu ms, expected ~%llu ms", (unsigned long long)(remaining / 1000000), (unsigned long long)(expected / 1000000));
    if (elapsed < 299000000ULL || elapsed > 300000000ULL + late_limit_ns) fail("SIGALRM interrupted the sleep after %llu us (300000 expected)", (unsigned long long)(elapsed / 1000));
    say("nanosleep(3600 s) interrupted after %llu us, %lld s left", (unsigned long long)(elapsed / 1000), (long long)left.tv_sec);
    alarms = 0; setitimer(ITIMER_REAL, &it, NULL); start = mono();
    if (poll(NULL, 0, 0x7FFFFFFF) != -1 || errno != EINTR) fail("poll(INT_MAX ms) not interrupted with EINTR");
    if (mono() - start > 300000000ULL + late_limit_ns) fail("poll(INT_MAX ms) interrupted late");
    alarms = 0; setitimer(ITIMER_REAL, &it, NULL); start = mono();
    if (poll(NULL, 0, -1) != -1 || errno != EINTR) fail("poll(-1) not interrupted with EINTR");

    /* 24 threads with distinct deadlines 3..95 ms, all at once: every one wakes on time */
    enum { N = 24 }; pthread_t threads[N]; struct sleeper sleepers[N]; struct stats st = {.name = "concurrent"};
    for (unsigned i = 0; i < N; i++) { sleepers[i].delay = 3000000ULL + (uint64_t)((i * 7) % N) * 4000000ULL; pthread_create(&threads[i], NULL, sleeper_main, &sleepers[i]); }
    for (unsigned i = 0; i < N; i++) { pthread_join(threads[i], NULL); stats_add(&st, sleepers[i].delay, sleepers[i].elapsed); }
    stats_report(&st);
    /* setitimer with an absurd value saturates instead of wrapping; disarming works */
    struct itimerval huge = {{0, 0}, {0x7FFFFFFFFFFFFFFLL, 0}}, back;
    if (setitimer(ITIMER_REAL, &huge, NULL)) fail("setitimer huge: %s", strerror(errno));
    if (getitimer(ITIMER_REAL, &back) || (back.it_value.tv_sec == 0 && back.it_value.tv_usec == 0)) fail("huge ITIMER_REAL reads as disarmed");
    setitimer(ITIMER_REAL, &(struct itimerval){{0, 0}, {0, 0}}, NULL);
    struct itimerval badit = {{0, 0}, {0, 1000000}}; if (setitimer(ITIMER_REAL, &badit, NULL) != -1 || errno != EINVAL) fail("setitimer tv_usec=1e6 accepted");
}

/* ---- busy CPUs ---- */
static volatile bool spin_stop; static volatile uint64_t spins;
static void* spinner(void* unused) { (void)unused; while (!spin_stop) spins++; return NULL; }
static void busy(void) {
    unsigned cpus = online_cpus(); pthread_t spin[CPUS]; unsigned spinners = cpus > 1 ? cpus - 1 : 1;
    spin_stop = false;
    for (unsigned i = 0; i < spinners; i++) pthread_create(&spin[i], NULL, spinner, NULL);
    struct stats st = {.name = "sleep_beside_spinners"};
    for (unsigned i = 0; i < 40; i++) { uint64_t d = 1000000ULL + (i % 5) * 3000000ULL, start = mono(); struct timespec t = ts(d); nanosleep(&t, NULL); stats_add(&st, d, mono() - start); }
    spin_stop = true; for (unsigned i = 0; i < spinners; i++) pthread_join(spin[i], NULL);
    stats_report(&st); say("spinners=%u spins=%llu", spinners, (unsigned long long)spins);
    /* alternate busy and idle phases: the tick stops and restarts many times */
    struct stats alt = {.name = "busy_idle_alternation"};
    for (unsigned i = 0; i < 30; i++) {
        uint64_t until = mono() + 3000000ULL; while (mono() < until) {}
        uint64_t d = 7000000ULL + (i % 3) * 11000000ULL, start = mono(); struct timespec t = ts(d); nanosleep(&t, NULL); stats_add(&alt, d, mono() - start);
    }
    stats_report(&alt);
}

int main(int argc, char** argv) {
    char config[256] = {0};
    int fd = open("/nexus/test-power", O_RDONLY);
    if (fd >= 0) { ssize_t n = read(fd, config, sizeof(config) - 1); if (n > 0) config[n] = 0; close(fd); }
    const char* modes = config[0] == 't' && config[1] == ' ' ? config + 2 : argc > 1 ? argv[1] : "a h b m5";
    if (getenv("TICK_LATE_LIMIT_MS")) late_limit_ns = (uint64_t)atoll(getenv("TICK_LATE_LIMIT_MS")) * 1000000ULL;
    say("START modes=%s cpus=%u", modes, online_cpus());
    char copy[256]; snprintf(copy, sizeof(copy), "%s", modes);
    for (char* m = strtok(copy, " \n"); m; m = strtok(NULL, " \n")) {
        uint64_t start = mono();
        switch (m[0]) {
        case 'm': measure(m[1] ? (unsigned)atoi(m + 1) : 5); break;
        case 'a': accuracy(); break;
        case 'h': hostile(); break;
        case 'b': busy(); break;
        case 'p': audio(m[1] ? (unsigned)atoi(m + 1) : 6); break;
        case 'P': audio_sleep(); break;
        default: fail("unknown mode %s", m);
        }
        say("MODE %c done in %llu ms", m[0], (unsigned long long)((mono() - start) / 1000000));
    }
    say("DONE failures=%d", failures);
    return failures ? 1 : 0;
}
