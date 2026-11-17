/* Early-boot console prompts, before any process runs: the in-kernel installer's confirmation and passphrase
 * (osinstall.c), the encrypted-root and crypttab passphrases (cryptroot.c, crypttab.c). Keys come from the input
 * layer: the PS/2 controller is polled (input_irq) and the USB worker is driven (usb_drive) so USB HID keyboards
 * work too. Characters follow the configured keyboard layout (keymaps.c: Shift, AltGr = right Alt, Caps Lock, dead
 * keys) and are stored as UTF-8. Backspace removes one character, Enter accepts, Escape cancels, Ctrl+U clears.
 * Secret input echoes '*'; the buffer is the caller's. */
#include "native.h"
#include "../../kernel/vga.h"
#include "storage_util.h"
#include "keymaps.h"

bool input_next_key(unsigned index, uint64_t* sequence, unsigned* code, int* value);
uint64_t input_sequence(unsigned index);
void usb_drive(void);

struct line { char* out; unsigned capacity, bytes, chars; bool shift, caps, control, altgr; struct keymap_state dead; };
enum { KEY_NONE, KEY_TEXT, KEY_ERASE, KEY_CLEAR, KEY_ENTER, KEY_ESCAPE };
/* one key event: edits the line; KEY_TEXT when characters were added */
static int key(struct line* l, unsigned code, int value) {
    if (code == 42 || code == 54) { l->shift = value != 0; return KEY_NONE; }
    if (code == 29 || code == 97) { l->control = value != 0; return KEY_NONE; }
    if (code == 100) { l->altgr = value != 0; return KEY_NONE; }   /* right Alt: AltGr (level 3) */
    if (code == 58 && value == 1) { l->caps = !l->caps; return KEY_NONE; }
    if (!value) return KEY_NONE;
    if (code == 1) { l->dead.dead = 0; return KEY_ESCAPE; }
    if (code == 28 || code == 96) { l->dead.dead = 0; return KEY_ENTER; }   /* Enter, keypad Enter */
    if (code == 14) {   /* Backspace: a pending dead key, else the last character (all its UTF-8 bytes) */
        if (l->dead.dead) { l->dead.dead = 0; return KEY_NONE; }
        if (!l->bytes) return KEY_NONE;
        do l->bytes--; while (l->bytes && ((uint8_t)l->out[l->bytes] & 0xC0) == 0x80);
        l->out[l->bytes] = 0; l->chars--; return KEY_ERASE;
    }
    if (l->control && code == 22) { memset(l->out, 0, l->capacity); l->bytes = l->chars = 0; l->dead.dead = 0; return KEY_CLEAR; }   /* Ctrl+U */
    if (l->control) return KEY_NONE;
    uint32_t cps[2]; unsigned n;
    if (code == 55) { cps[0] = '*'; n = 1; }   /* keypad '*' */
    else n = keymap_translate(&l->dead, code, l->shift, l->altgr, l->caps, cps);
    bool added = false;
    for (unsigned i = 0; i < n; i++) {
        char utf8[4]; unsigned bytes = keymap_utf8(cps[i], utf8);
        if (cps[i] < 0x20 || l->bytes + bytes + 1 > l->capacity) continue;
        memcpy(l->out + l->bytes, utf8, bytes); l->bytes += bytes; l->out[l->bytes] = 0; l->chars++; added = true;
    }
    return added ? KEY_TEXT : KEY_NONE;
}
static void begin(struct line* l, char* out, unsigned capacity) { memset(l, 0, sizeof(*l)); l->out = out; l->capacity = capacity; out[0] = 0; }
static void echo_from(const char* out, unsigned from, unsigned to, bool secret) {   /* the console font is ASCII */
    for (unsigned i = from; i < to; i++) if (((uint8_t)out[i] & 0xC0) != 0x80) {
        char c[2] = {secret ? '*' : (uint8_t)out[i] < 0x80 ? out[i] : '?', 0}; vga_print(c);
    }
}

/* A secret line for a graphical prompt (cryptroot.c): `changed(characters, ctx)` redraws after every edit; nothing
 * is echoed on the console. Enter returns the length in bytes, Escape -1. */
int console_read_secret(char* out, unsigned capacity, void (*changed)(unsigned length, void* ctx), void* ctx) {
    if (!capacity) return -1;
    uint64_t seen[2] = {input_sequence(0), input_sequence(1)};
    struct line l; begin(&l, out, capacity);
    for (;;) {
        input_irq(); usb_drive();
        unsigned code; int value;
        while (input_next_key(0, &seen[0], &code, &value)) {
            int r = key(&l, code, value);
            if (r == KEY_ESCAPE) { memset(out, 0, capacity); return -1; }
            if (r == KEY_ENTER) return (int)l.bytes;
            if (r != KEY_NONE) changed(l.chars, ctx);
        }
        unsigned dc; int dv;
        while (input_next_key(1, &seen[1], &dc, &dv)) {}
        for (uint64_t until = native_clock_ns() + 2000000ULL; native_clock_ns() < until; ) __asm__ volatile("pause");
    }
}
/* Returns the length typed (bytes), or -1 (Escape, or no key for timeout_ns; 0 = wait forever). Non-ASCII
 * characters echo as '?'. */
int console_prompt(const char* prompt, char* out, unsigned capacity, bool secret, uint64_t timeout_ns) {
    if (!capacity) return -1;
    uint64_t seen[2] = {input_sequence(0), input_sequence(1)};
    struct line l; begin(&l, out, capacity);
    vga_print(prompt);
    uint64_t last = native_clock_ns();
    for (;;) {
        input_irq(); usb_drive();
        unsigned code; int value; bool got = false;
        while (input_next_key(0, &seen[0], &code, &value)) {
            got = true;
            unsigned before = l.bytes;
            int r = key(&l, code, value);
            if (r == KEY_ESCAPE) { vga_print("\n"); memset(out, 0, capacity); return -1; }
            if (r == KEY_ENTER) { vga_print("\n"); return (int)l.bytes; }
            if (r == KEY_ERASE || r == KEY_CLEAR) {   /* the console has no backspace: redraw the line with blanks after it */
                unsigned blanks = r == KEY_CLEAR ? 64 : 1;
                vga_print("\r"); vga_print(prompt); echo_from(out, 0, l.bytes, secret);
                for (unsigned i = 0; i < blanks; i++) vga_print(" ");
                vga_print("\r"); vga_print(prompt); echo_from(out, 0, l.bytes, secret);
            } else if (r == KEY_TEXT) echo_from(out, before, l.bytes, secret);
        }
        unsigned dc; int dv;   /* keep the mouse queue drained */
        while (input_next_key(1, &seen[1], &dc, &dv)) {}
        uint64_t now = native_clock_ns();
        if (got) last = now;
        else if (timeout_ns && now - last >= timeout_ns) { vga_print("\n"); memset(out, 0, capacity); return -1; }
        for (uint64_t until = now + 2000000ULL; native_clock_ns() < until; ) __asm__ volatile("pause");
    }
}
