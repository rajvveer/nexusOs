/* Aurora sound devices (aurora_sound.c): which ALSA card "default" means, like the sound settings of macOS/GNOME.
 *
 * NexusOS mixes ALSA clients in the kernel (sound.c), so the default device is plain ALSA configuration: Aurora writes
 * AURORA_SOUND_CONFIG (in the runtime directory) with pcm.!default = asym of plughw:CARD=<output> / plughw:CARD=<input>
 * and ctl.!default = the output card, by card id (stable across hot-plug; indexes are not), and puts that file into
 * ALSA_CONFIG_PATH (after alsa.conf) for every program it starts: alsa-lib re-reads the files on that path when they
 * change, so the next snd_pcm_open of a running program (Chrome, Firefox, aplay) follows a new choice. A default the
 * user defined in /etc/asound.conf or ~/.asoundrc wins (Aurora then writes no default). The choice per direction is a
 * card id or "auto" (the device plugged in most recently, else the first card), kept in AURORA_SOUND_PREFERENCES.
 * USB audio hot-plug (SUBSYSTEM=sound uevents) re-applies it and shows a notice. Control socket: "sound" (cards +
 * current choice), "set-sound output=ID|auto input=ID|auto". */
#ifndef AURORA_SOUND_H
#define AURORA_SOUND_H
#include <stdbool.h>
#include <stddef.h>

#define AURORA_SOUND_CARDS 8
#define AURORA_SOUND_CONFIG "/run/user/1000/asound.conf"
#define AURORA_SOUND_PREFERENCES "/etc/nexus-sound.conf"
#define AURORA_SOUND_ALSA_PATH "/usr/share/alsa/alsa.conf:" AURORA_SOUND_CONFIG
struct aurora_sound_card { int index; char id[16], name[32], longname[80], driver[16]; bool playback, capture; unsigned plugged; };
struct aurora_sound_state {
    struct aurora_sound_card cards[AURORA_SOUND_CARDS]; int count;
    char want_output[16], want_input[16];       /* "auto" or a card id */
    char output[16], input[16];                 /* what the configuration says now ("" = none) */
    bool foreign;                               /* the user defined a default in /etc/asound.conf or ~/.asoundrc */
    int error;                                  /* errno of the last failed write (0: written / unchanged) */
};
void aurora_sound_scan(struct aurora_sound_state* state);
/* Preferences (want_output/want_input) from `preferences`; foreign from `user_configs` (NULL-terminated paths). */
void aurora_sound_load(struct aurora_sound_state* state, const char* preferences, const char* const* user_configs);
/* Scan, choose and write `config` (and the preferences, best effort); true when the output or input changed. */
bool aurora_sound_apply(struct aurora_sound_state* state, const char* config, const char* preferences);
const struct aurora_sound_card* aurora_sound_find(const struct aurora_sound_state* state, const char* id);
#ifndef AURORA_SOUND_STANDALONE
struct wl_event_loop;
void aurora_sound_init(struct wl_event_loop* loop, void (*notify)(const char* text));
#endif
#endif
