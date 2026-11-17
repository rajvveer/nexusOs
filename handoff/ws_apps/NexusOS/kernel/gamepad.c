/* ============================================================================
 * NexusOS — Game Controller Layer (Implementation) — Phase 42
 * ============================================================================
 * Tracks held-key state from raw PS/2 make/break codes (via the keyboard
 * driver's raw hook) and folds them into a 12-button virtual game pad.
 * State updates happen in IRQ1 context and are single byte/word writes,
 * so no locking is needed against the game loop.
 * ============================================================================ */

#include "gamepad.h"
#include "keyboard.h"
#include "vga.h"

/* Held state for every key: [0..127] = plain, [128..255] = E0-extended */
static volatile uint8_t key_state[256];

/* Virtual pad state */
static volatile uint16_t pad_held;       /* live bitmask (IRQ-updated)      */
static uint16_t pad_latched;             /* snapshot from gamepad_poll()    */
static uint16_t pad_edges;               /* went-down edges since last poll */
static volatile uint16_t pad_edge_accum; /* IRQ-side edge accumulator       */
static bool acquired = false;

/* SDL-style event ring */
#define PAD_EVQ_SIZE 32
static pad_event_t evq[PAD_EVQ_SIZE];
static volatile int evq_head, evq_tail;

/* --------------------------------------------------------------------------
 * Key → button mapping. Each button has up to 3 source keys.
 * Scancode set 1; 0x100 flag = E0-extended.
 * -------------------------------------------------------------------------- */
#define EXT 0x100
typedef struct {
    uint16_t    keys[3];
    const char* name;
    const char* keystr;
} pad_map_t;

static const pad_map_t pad_map[PAD_BTN_COUNT] = {
    [PAD_UP]     = { { EXT|0x48, 0x11, 0    }, "UP",     "Up / W"      },
    [PAD_DOWN]   = { { EXT|0x50, 0x1F, 0    }, "DOWN",   "Down / S"    },
    [PAD_LEFT]   = { { EXT|0x4B, 0,    0    }, "LEFT",   "Left"        },
    [PAD_RIGHT]  = { { EXT|0x4D, 0,    0    }, "RIGHT",  "Right"       },
    [PAD_A]      = { { 0x1D, 0x2D, 0         }, "A",      "Ctrl / X"   },
    [PAD_B]      = { { 0x39, 0x2C, 0         }, "B",      "Space / Z"  },
    [PAD_X]      = { { 0x2E, 0, 0            }, "X",      "C"          },
    [PAD_Y]      = { { 0x2F, 0x32, 0         }, "Y",      "V / M"      },
    [PAD_L]      = { { 0x1E, 0x10, 0         }, "L",      "A / Q"      },
    [PAD_R]      = { { 0x20, 0x12, 0         }, "R",      "D / E"      },
    [PAD_START]  = { { 0x1C, 0, 0            }, "START",  "Enter"      },
    [PAD_SELECT] = { { 0x01, 0x0F, 0         }, "SELECT", "Esc / Tab"  },
};

/* --------------------------------------------------------------------------
 * IRQ-side helpers
 * -------------------------------------------------------------------------- */
static void evq_push(uint8_t btn, bool down) {
    int next = (evq_head + 1) % PAD_EVQ_SIZE;
    if (next == evq_tail) return;                /* full — drop oldest-first */
    evq[evq_head].btn  = btn;
    evq[evq_head].down = down;
    evq_head = next;
}

/* Recompute one button's level from its source keys; emit edge events. */
static void pad_update_btn(int btn) {
    bool down = false;
    for (int i = 0; i < 3 && pad_map[btn].keys[i]; i++) {
        uint16_t k = pad_map[btn].keys[i];
        if (key_state[(k & 0x7F) | ((k & EXT) ? 0x80 : 0)]) { down = true; break; }
    }
    uint16_t bit = (uint16_t)(1u << btn);
    bool was = (pad_held & bit) != 0;
    if (down == was) return;
    if (down) { pad_held |= bit; pad_edge_accum |= bit; evq_push((uint8_t)btn, true); }
    else      { pad_held &= (uint16_t)~bit; evq_push((uint8_t)btn, false); }
}

/* Raw keyboard observer — runs in IRQ1 context. Exposed so another driver
 * (the Phase 43 accessibility layer) can own the single keyboard raw-hook slot
 * and forward every scancode here, keeping the controller layer alive. Returns
 * true when the pad consumes the key (only while a game owns the pad). */
bool gamepad_raw_observe(uint8_t scancode, bool ext, bool released) {
    uint8_t idx = (uint8_t)(scancode | (ext ? 0x80 : 0));
    key_state[idx] = released ? 0 : 1;

    /* Fold into whichever buttons this key feeds */
    uint16_t k = (uint16_t)(scancode | (ext ? EXT : 0));
    for (int b = 0; b < PAD_BTN_COUNT; b++) {
        for (int i = 0; i < 3 && pad_map[b].keys[i]; i++) {
            if (pad_map[b].keys[i] == k) { pad_update_btn(b); break; }
        }
    }
    return acquired;            /* consume keys only while a game owns us */
}

/* Raw keyboard hook — runs in IRQ1 context. */
static bool pad_raw_hook(uint8_t scancode, bool ext, bool released) {
    return gamepad_raw_observe(scancode, ext, released);
}

/* --------------------------------------------------------------------------
 * Public API
 * -------------------------------------------------------------------------- */
void gamepad_init(void) {
    for (int i = 0; i < 256; i++) key_state[i] = 0;
    pad_held = 0; pad_latched = 0; pad_edges = 0; pad_edge_accum = 0;
    evq_head = evq_tail = 0;
    keyboard_set_raw_hook(pad_raw_hook);

    vga_print_color("[OK] ", VGA_COLOR(VGA_LIGHT_GREEN, VGA_BLACK));
    vga_print("Game controller layer initialized (12-button virtual pad)\n");
}

void gamepad_acquire(void) {
    for (int i = 0; i < 256; i++) key_state[i] = 0;
    pad_held = 0; pad_latched = 0; pad_edges = 0; pad_edge_accum = 0;
    evq_head = evq_tail = 0;
    acquired = true;
}

void gamepad_release(void) {
    acquired = false;
    pad_held = 0; pad_latched = 0; pad_edges = 0; pad_edge_accum = 0;
    for (int i = 0; i < 256; i++) key_state[i] = 0;
    /* Drain anything that reached the character buffer around the edges */
    while (keyboard_has_key()) keyboard_getchar();
}

bool gamepad_acquired(void) { return acquired; }

void gamepad_poll(void) {
    /* Edges = accumulated IRQ-side presses since the previous poll, so even
     * a press+release inside one frame still registers once. */
    pad_edges = pad_edge_accum;
    pad_edge_accum = 0;
    pad_latched = pad_held;
}

uint16_t gamepad_buttons(void) { return pad_held; }

bool gamepad_held(int btn) {
    return (pad_held & (1u << btn)) != 0;
}

bool gamepad_pressed(int btn) {
    return (pad_edges & (1u << btn)) != 0;
}

bool gamepad_next_event(pad_event_t* ev) {
    if (evq_tail == evq_head) return false;
    *ev = evq[evq_tail];
    evq_tail = (evq_tail + 1) % PAD_EVQ_SIZE;
    return true;
}

bool gamepad_key_held(uint8_t scancode, bool ext) {
    return key_state[(scancode & 0x7F) | (ext ? 0x80 : 0)] != 0;
}

void gamepad_feed(uint8_t btn, bool down) {
    if (btn >= PAD_BTN_COUNT) return;
    uint16_t bit = (uint16_t)(1u << btn);
    bool was = (pad_held & bit) != 0;
    if (down == was) return;
    if (down) { pad_held |= bit; pad_edge_accum |= bit; evq_push(btn, true); }
    else      { pad_held &= (uint16_t)~bit; evq_push(btn, false); }
}

const char* gamepad_name(void) {
    return "PS/2 keyboard pad (12 buttons)";
}

const char* gamepad_btn_name(int btn) {
    if (btn < 0 || btn >= PAD_BTN_COUNT) return "?";
    return pad_map[btn].name;
}

const char* gamepad_btn_keys(int btn) {
    if (btn < 0 || btn >= PAD_BTN_COUNT) return "?";
    return pad_map[btn].keystr;
}
