/* ============================================================================
 * NexusOS — Music Player (Implementation)
 * ============================================================================
 * PC speaker music player with preset melodies.
 * ============================================================================ */

#include "music.h"
#include "window.h"
#include "gui.h"
#include "theme.h"
#include "vga.h"
#include "string.h"
#include "speaker.h"
#include "keyboard.h"

extern volatile uint32_t system_ticks;

/* Note frequencies (Hz) */
#define C4  262
#define D4  294
#define E4  330
#define F4  349
#define G4  392
#define A4  440
#define B4  494
#define C5  523
#define D5  587
#define E5  659
#define F5  698
#define G5  784
#define A5  880
#define R   0     /* Rest */

typedef struct { uint16_t freq; uint8_t dur; } note_t;

/* Tetris Theme (Korobeiniki) - simplified */
static const note_t song_tetris[] = {
    {E5,4},{B4,2},{C5,2},{D5,4},{C5,2},{B4,2},
    {A4,4},{A4,2},{C5,2},{E5,4},{D5,2},{C5,2},
    {B4,4},{B4,2},{C5,2},{D5,4},{E5,4},
    {C5,4},{A4,4},{A4,4},{R,4},
    {D5,4},{F5,2},{A5,4},{G5,2},{F5,2},
    {E5,4},{C5,2},{E5,4},{D5,2},{C5,2},
    {B4,4},{B4,2},{C5,2},{D5,4},{E5,4},
    {C5,4},{A4,4},{A4,4},{R,4},
    {0,0}
};

/* Fur Elise - opening */
static const note_t song_elise[] = {
    {E5,2},{D5,2},{E5,2},{D5,2},{E5,2},{B4,2},{D5,2},{C5,2},
    {A4,4},{R,2},{C4,2},{E4,2},{A4,2},
    {B4,4},{R,2},{E4,2},{G4,2},{B4,2},
    {C5,4},{R,2},{E4,2},{E5,2},{D5,2},
    {E5,2},{D5,2},{E5,2},{B4,2},{D5,2},{C5,2},
    {A4,4},{R,4},
    {0,0}
};

/* Simple Scale */
static const note_t song_scale[] = {
    {C4,3},{D4,3},{E4,3},{F4,3},{G4,3},{A4,3},{B4,3},{C5,3},
    {C5,3},{B4,3},{A4,3},{G4,3},{F4,3},{E4,3},{D4,3},{C4,3},
    {R,4},
    {0,0}
};

/* Mario Theme - simplified */
static const note_t song_mario[] = {
    {E5,2},{E5,2},{R,2},{E5,2},{R,2},{C5,2},{E5,4},
    {G5,4},{R,4},{G4,4},{R,4},
    {C5,4},{R,2},{G4,4},{R,2},{E4,4},
    {R,2},{A4,4},{B4,4},{A4,2},{A4,4},
    {G4,3},{E5,3},{G5,3},{A5,4},{F5,2},{G5,2},
    {R,2},{E5,4},{C5,2},{D5,2},{B4,4},{R,4},
    {0,0}
};

/* Ode to Joy */
static const note_t song_joy[] = {
    {E4,3},{E4,3},{F4,3},{G4,3},{G4,3},{F4,3},{E4,3},{D4,3},
    {C4,3},{C4,3},{D4,3},{E4,3},{E4,4},{D4,2},{D4,4},{R,2},
    {E4,3},{E4,3},{F4,3},{G4,3},{G4,3},{F4,3},{E4,3},{D4,3},
    {C4,3},{C4,3},{D4,3},{E4,3},{D4,4},{C4,2},{C4,4},{R,4},
    {0,0}
};

typedef struct {
    const char* name;
    const note_t* notes;
} track_t;

static const track_t tracks[] = {
    { "Tetris Theme",  song_tetris },
    { "Fur Elise",     song_elise  },
    { "Scale Up/Down", song_scale  },
    { "Mario Theme",   song_mario  },
    { "Ode to Joy",    song_joy    },
};
#define TRACK_COUNT 5

static int mus_track = 0;
static bool mus_playing = false;
static int mus_note_idx = 0;
static uint32_t mus_next_tick = 0;

static void mus_draw(int id, int cx, int cy, int cw, int ch) {
    (void)id;
    const theme_t* t = theme_get();
    uint8_t tc = t->win_content;
    uint8_t bg = (tc >> 4) & 0x0F;
    uint8_t accent = VGA_COLOR(VGA_LIGHT_CYAN, bg);
    uint8_t dim = VGA_COLOR(VGA_DARK_GREY, bg);
    uint8_t hi = t->menu_highlight;
    uint8_t play_col = VGA_COLOR(VGA_LIGHT_GREEN, bg);

    int row = cy;
    gui_draw_text(cx + 1, row, "\x0E Music Player", accent);
    row += 2;

    /* Track list */
    for (int i = 0; i < TRACK_COUNT && row < cy + ch - 4; i++) {
        bool sel = (i == mus_track);
        uint8_t col = sel ? hi : tc;
        if (sel) for (int j = cx; j < cx + cw - 1; j++) gui_putchar(j, row, ' ', hi);
        gui_putchar(cx + 1, row, sel ? '\x10' : ' ', col);
        gui_draw_text(cx + 3, row, tracks[i].name, col);
        if (sel && mus_playing) gui_draw_text(cx + cw - 4, row, "\x0E", play_col);
        row++;
    }

    row++;
    /* Now playing */
    if (mus_playing) {
        gui_draw_text(cx + 1, row, "Now Playing:", play_col);
        row++;
        gui_draw_text(cx + 3, row, tracks[mus_track].name, accent);
        row++;

        /* Progress bar */
        int total = 0;
        for (int i = 0; tracks[mus_track].notes[i].dur != 0 || tracks[mus_track].notes[i].freq != 0; i++) total++;
        int bar_w = cw - 4;
        int filled = total > 0 ? (mus_note_idx * bar_w) / total : 0;
        if (filled > bar_w) filled = bar_w;
        gui_putchar(cx + 1, row, '[', dim);
        for (int i = 0; i < bar_w; i++)
            gui_putchar(cx + 2 + i, row, i < filled ? (char)0xDB : (char)0xB0, i < filled ? play_col : dim);
        gui_putchar(cx + 2 + bar_w, row, ']', dim);
    } else {
        gui_draw_text(cx + 1, row, "Stopped", dim);
    }

    int hint_row = cy + ch - 1;
    gui_draw_text(cx + 1, hint_row, "Space:Play/Stop  Up/Dn", dim);
    (void)cw;
}

static void mus_key(int id, char key) {
    (void)id;
    if ((unsigned char)key == 0x80 && mus_track > 0) mus_track--;
    if ((unsigned char)key == 0x81 && mus_track < TRACK_COUNT - 1) mus_track++;

    if (key == ' ') {
        if (mus_playing) {
            mus_playing = false;
            beep_stop();
        } else {
            mus_playing = true;
            mus_note_idx = 0;
            mus_next_tick = system_ticks;
        }
    }

    /* Play current note if playing */
    if (mus_playing && system_ticks >= mus_next_tick) {
        const note_t* n = &tracks[mus_track].notes[mus_note_idx];
        if (n->dur == 0 && n->freq == 0) {
            mus_playing = false;
            beep_stop();
            return;
        }
        if (n->freq > 0) beep(n->freq, n->dur * 3);
        else beep_stop();
        mus_next_tick = system_ticks + n->dur * 3;
        mus_note_idx++;
    }
}

int music_open(void) {
    mus_track = 0; mus_playing = false; mus_note_idx = 0;
    return window_create("Music Player", 22, 3, 30, 16, mus_draw, mus_key);
}
