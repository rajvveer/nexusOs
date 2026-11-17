/* Aurora sound devices: the default ALSA output/input and USB audio hot-plug (see aurora_sound.h). The policy part
 * (scan, choice, configuration files) builds standalone too (-DAURORA_SOUND_STANDALONE: the guest test of
 * tests/test_x64_usbaudio.py runs it against the real kernel and alsa-lib without a compositor). */
#define _GNU_SOURCE
#include <errno.h>
#include <fcntl.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/stat.h>
#include <unistd.h>
#include "aurora_sound.h"

#define HEADER "# NexusOS default sound devices, written by Aurora (Settings > Sound); regenerated on every change.\n"
struct card_info { int32_t card, pad; char id[16], driver[16], name[32], longname[80], reserved[16], mixername[80], components[128]; };
#define CARD_INFO _IOR('U', 0x01, struct card_info)
_Static_assert(sizeof(struct card_info) == 376, "snd_ctl_card_info");
static unsigned plug_clock;

static bool exists(const char* path) { struct stat st; return !stat(path, &st) && S_ISCHR(st.st_mode); }
static bool valid_id(const char* id) {
    if (!id[0] || strlen(id) > 15) return false;
    for (const char* p = id; *p; p++) if (!((*p >= 'A' && *p <= 'Z') || (*p >= 'a' && *p <= 'z') || (*p >= '0' && *p <= '9') || *p == '_' || *p == '-')) return false;
    return true;
}
void aurora_sound_scan(struct aurora_sound_state* state) {
    struct aurora_sound_card previous[AURORA_SOUND_CARDS]; int before = state->count;
    memcpy(previous, state->cards, sizeof(previous)); state->count = 0;
    for (int n = 0; n < AURORA_SOUND_CARDS; n++) {
        char path[64]; snprintf(path, sizeof(path), "/dev/snd/controlC%d", n);
        int fd = open(path, O_RDONLY | O_CLOEXEC | O_NONBLOCK);
        if (fd < 0) continue;
        struct card_info info; memset(&info, 0, sizeof(info));
        int result = ioctl(fd, CARD_INFO, &info); close(fd);
        if (result) continue;
        struct aurora_sound_card* c = &state->cards[state->count]; memset(c, 0, sizeof(*c));
        c->index = n;
        snprintf(c->id, sizeof(c->id), "%.15s", info.id); snprintf(c->name, sizeof(c->name), "%.31s", info.name);
        snprintf(c->longname, sizeof(c->longname), "%.79s", info.longname); snprintf(c->driver, sizeof(c->driver), "%.15s", info.driver);
        if (!valid_id(c->id)) continue;                /* never written into the configuration */
        snprintf(path, sizeof(path), "/dev/snd/pcmC%dD0p", n); c->playback = exists(path);
        snprintf(path, sizeof(path), "/dev/snd/pcmC%dD0c", n); c->capture = exists(path);
        for (int k = 0; k < before; k++) if (!strcmp(previous[k].id, c->id)) c->plugged = previous[k].plugged;
        if (!c->plugged) c->plugged = ++plug_clock;   /* first seen now (cards present at start keep index order) */
        state->count++;
    }
}
const struct aurora_sound_card* aurora_sound_find(const struct aurora_sound_state* state, const char* id) {
    for (int i = 0; i < state->count; i++) if (!strcmp(state->cards[i].id, id)) return &state->cards[i];
    return NULL;
}
/* The chosen card when present, else the most recently plugged card with that direction. */
static void choose(const struct aurora_sound_state* state, const char* want, bool capture, char* out) {
    const struct aurora_sound_card* best = NULL;
    const struct aurora_sound_card* wanted = strcmp(want, "auto") ? aurora_sound_find(state, want) : NULL;
    if (wanted && (capture ? wanted->capture : wanted->playback)) best = wanted;
    if (!best) for (int i = 0; i < state->count; i++) {
        const struct aurora_sound_card* c = &state->cards[i];
        if ((capture ? c->capture : c->playback) && (!best || c->plugged > best->plugged)) best = c;
    }
    snprintf(out, 16, "%s", best ? best->id : "");
}
static char* read_file(const char* path) {
    FILE* f = fopen(path, "re"); if (!f) return NULL;
    char* text = calloc(1, 16385); size_t n = text ? fread(text, 1, 16384, f) : 0; fclose(f);
    if (text) text[n] = 0;
    return text;
}
/* A user's own default: "pcm.!default", "pcm.default" or "defaults.pcm.card" at the start of a line, outside comments. */
static bool defines_default(const char* text) {
    for (const char* line = text; line && *line; ) {
        while (*line == ' ' || *line == '\t') line++;
        if (*line != '#' && (!strncmp(line, "pcm.!default", 12) || !strncmp(line, "pcm.default", 11) || !strncmp(line, "defaults.pcm.card", 17))) return true;
        const char* next = strchr(line, '\n'); line = next ? next + 1 : NULL;
    }
    return false;
}
void aurora_sound_load(struct aurora_sound_state* state, const char* preferences, const char* const* user_configs) {
    state->foreign = false;
    for (unsigned i = 0; user_configs && user_configs[i]; i++) {
        char* text = read_file(user_configs[i]);
        if (text && defines_default(text)) state->foreign = true;
        free(text);
    }
    if (!preferences) return;
    snprintf(state->want_output, sizeof(state->want_output), "auto"); snprintf(state->want_input, sizeof(state->want_input), "auto");
    char* text = read_file(preferences);
    if (!text) return;
    char value[32]; char* p;
    if ((p = strstr(text, "output=")) && sscanf(p + 7, "%31[A-Za-z0-9_-]", value) == 1 && (valid_id(value) || !strcmp(value, "auto"))) snprintf(state->want_output, 16, "%s", value);
    if ((p = strstr(text, "input=")) && sscanf(p + 6, "%31[A-Za-z0-9_-]", value) == 1 && (valid_id(value) || !strcmp(value, "auto"))) snprintf(state->want_input, 16, "%s", value);
    free(text);
}
static bool replace_file(const char* path, const char* text, int* error) {
    char* old = read_file(path);
    bool same = old && !strcmp(old, text); free(old);
    if (same) return true;
    char temporary[256]; snprintf(temporary, sizeof(temporary), "%s.aurora-new", path);
    FILE* f = fopen(temporary, "we");
    if (!f || fputs(text, f) < 0 || fflush(f) || fsync(fileno(f))) { *error = errno; if (f) fclose(f); unlink(temporary); return false; }
    fclose(f); chmod(temporary, 0644);
    if (rename(temporary, path)) { *error = errno; unlink(temporary); return false; }   /* a new inode: alsa-lib sees the change */
    return true;
}
bool aurora_sound_apply(struct aurora_sound_state* state, const char* config, const char* preferences) {
    if (!state->want_output[0]) snprintf(state->want_output, sizeof(state->want_output), "auto");
    if (!state->want_input[0]) snprintf(state->want_input, sizeof(state->want_input), "auto");
    aurora_sound_scan(state);
    char output[16], input[16];
    choose(state, state->want_output, false, output); choose(state, state->want_input, true, input);
    if (state->foreign) output[0] = input[0] = 0;     /* the user's own default wins */
    bool changed = strcmp(output, state->output) || strcmp(input, state->input);
    char text[1024]; int n = snprintf(text, sizeof(text), "%s", HEADER);
    if (output[0] || input[0]) {
        n += snprintf(text + n, sizeof(text) - n, "pcm.!default {\n\ttype asym\n");
        if (output[0]) n += snprintf(text + n, sizeof(text) - n, "\tplayback.pcm \"plughw:CARD=%s,DEV=0\"\n", output);
        if (input[0]) n += snprintf(text + n, sizeof(text) - n, "\tcapture.pcm \"plughw:CARD=%s,DEV=0\"\n", input);
        n += snprintf(text + n, sizeof(text) - n, "}\nctl.!default {\n\ttype hw\n\tcard \"%s\"\n}\n", output[0] ? output : input);
    }
    state->error = 0;
    if (!replace_file(config, text, &state->error)) return false;
    snprintf(state->output, sizeof(state->output), "%s", output); snprintf(state->input, sizeof(state->input), "%s", input);
    if (preferences) {
        char choice[64]; snprintf(choice, sizeof(choice), "output=%s\ninput=%s\n", state->want_output, state->want_input);
        int ignored = 0; replace_file(preferences, choice, &ignored);   /* best effort: a read-only root keeps it per session */
    }
    return changed;
}

#ifndef AURORA_SOUND_STANDALONE
#include <sys/socket.h>
#include <linux/netlink.h>
#include <wayland-server.h>
#include "aurora_control.h"
static struct aurora_sound_state state;
static void (*notify)(const char* text);
static int uevents = -1;
static const char* const USER_CONFIGS[] = {"/etc/asound.conf", "/home/nexus/.asoundrc", NULL};
static const char* name_of(const char* id) { const struct aurora_sound_card* c = aurora_sound_find(&state, id); return c ? c->name : id; }
static void announce(const char* what) {
    char text[200]; const char* dash = what ? " - " : ""; if (!what) what = "";
    if (state.output[0]) snprintf(text, sizeof(text), "%s%sSound output: %s", what, dash, name_of(state.output));
    else if (state.foreign) snprintf(text, sizeof(text), "%s%sSound output: as set in your ALSA configuration", what, dash);
    else snprintf(text, sizeof(text), "%s%sNo sound output device", what, dash);
    printf("[64] AURORA SOUND %s (output=%s input=%s)\n", text, state.output, state.input); fflush(stdout);
    if (notify) notify(text);
}
static void apply(void) { aurora_sound_load(&state, NULL, USER_CONFIGS); aurora_sound_apply(&state, AURORA_SOUND_CONFIG, AURORA_SOUND_PREFERENCES); }
static int readable(int fd, uint32_t mask, void* data) {
    (void)mask; (void)data; char buffer[4096]; bool sound = false; char what[16] = "";
    for (;;) {
        ssize_t n = recv(fd, buffer, sizeof(buffer) - 1, MSG_DONTWAIT);
        if (n <= 0) break;
        buffer[n] = 0;
        if (!memmem(buffer, (size_t)n, "SUBSYSTEM=sound", 15)) continue;
        sound = true;
        if (strstr(buffer, "/controlC")) snprintf(what, sizeof(what), "%s", !strncmp(buffer, "add@", 4) ? "connected" : !strncmp(buffer, "remove@", 7) ? "disconnected" : "");
    }
    if (!sound) return 0;
    struct aurora_sound_state before = state;
    apply();
    bool changed = strcmp(before.output, state.output) || strcmp(before.input, state.input);
    if (what[0]) {   /* name the device that came or went */
        const struct aurora_sound_card* newest = NULL;
        for (int i = 0; i < state.count; i++) if (!newest || state.cards[i].plugged > newest->plugged) newest = &state.cards[i];
        char text[120];
        if (!strcmp(what, "connected") && newest) snprintf(text, sizeof(text), "%s connected", newest->name);
        else {
            const char* gone = "Sound device"; for (int i = 0; i < before.count; i++) if (!aurora_sound_find(&state, before.cards[i].id)) gone = before.cards[i].name;
            snprintf(text, sizeof(text), "%s disconnected", gone);
        }
        announce(text);
    } else if (changed) announce(NULL);
    return 0;
}
static bool control_get(const char* args, const char* body, struct aurora_control_reply* reply, char* error, size_t size) {
    (void)args; (void)body; (void)error; (void)size;
    apply();
    for (int i = 0; i < state.count; i++) {
        const struct aurora_sound_card* c = &state.cards[i];
        aurora_control_print(reply, "card %d id=%s playback=%d capture=%d name=%s\n", c->index, c->id, c->playback, c->capture, c->name);
    }
    aurora_control_print(reply, "output=%s input=%s want_output=%s want_input=%s foreign=%d\n", state.output, state.input, state.want_output, state.want_input, state.foreign);
    return true;
}
static bool control_set(const char* args, const char* body, struct aurora_control_reply* reply, char* error, size_t size) {
    (void)body;
    char output[16], input[16]; snprintf(output, sizeof(output), "%s", state.want_output); snprintf(input, sizeof(input), "%s", state.want_input);
    for (const char* p = args; p && *p; ) {
        while (*p == ' ') p++;
        if (!*p) break;
        char key[16], value[32]; int used = 0;
        if (sscanf(p, "%15[a-z]=%31[A-Za-z0-9_-]%n", key, value, &used) != 2) { snprintf(error, size, "bad setting %.40s", p); return false; }
        if (strcmp(value, "auto") && !valid_id(value)) { snprintf(error, size, "bad card id"); return false; }
        if (!strcmp(key, "output")) snprintf(output, sizeof(output), "%s", value);
        else if (!strcmp(key, "input")) snprintf(input, sizeof(input), "%s", value);
        else { snprintf(error, size, "unknown setting %.15s", key); return false; }
        p += used;
    }
    snprintf(state.want_output, sizeof(state.want_output), "%s", output); snprintf(state.want_input, sizeof(state.want_input), "%s", input);
    aurora_sound_load(&state, NULL, USER_CONFIGS);
    if (!aurora_sound_apply(&state, AURORA_SOUND_CONFIG, AURORA_SOUND_PREFERENCES) && state.error) { snprintf(error, size, "cannot write %s: %s", AURORA_SOUND_CONFIG, strerror(state.error)); return false; }
    announce(NULL);
    return control_get("", NULL, reply, error, size);
}
void aurora_sound_init(struct wl_event_loop* loop, void (*notice)(const char* text)) {
    notify = notice;
    aurora_sound_load(&state, AURORA_SOUND_PREFERENCES, USER_CONFIGS);
    aurora_sound_apply(&state, AURORA_SOUND_CONFIG, AURORA_SOUND_PREFERENCES);
    if (!getenv("ALSA_CONFIG_PATH")) setenv("ALSA_CONFIG_PATH", AURORA_SOUND_ALSA_PATH, 1);   /* every program Aurora starts */
    aurora_control_register("sound", false, control_get);
    aurora_control_register("set-sound", false, control_set);
    printf("[64] AURORA SOUND ready output=%s input=%s cards=%d%s%s\n", state.output, state.input, state.count,
           state.foreign ? " (your ALSA configuration defines the default)" : "", state.error ? " (configuration not written)" : ""); fflush(stdout);
    uevents = socket(AF_NETLINK, SOCK_DGRAM | SOCK_CLOEXEC | SOCK_NONBLOCK, NETLINK_KOBJECT_UEVENT);
    struct sockaddr_nl address = {.nl_family = AF_NETLINK, .nl_groups = 1};
    if (uevents < 0 || bind(uevents, (struct sockaddr*)&address, sizeof(address)) || !wl_event_loop_add_fd(loop, uevents, WL_EVENT_READABLE, readable, NULL)) {
        perror("aurora sound: uevent listener"); if (uevents >= 0) close(uevents); uevents = -1;
    }
}
#endif
