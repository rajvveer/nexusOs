/* ============================================================================
 * NexusOS — PS/2 Keyboard Driver (Implementation)
 * ============================================================================
 * Handles IRQ1 (keyboard interrupt).
 * Converts scancodes to ASCII using a lookup table.
 * Supports shift, caps lock, and a key buffer.
 * ============================================================================ */

#include "keyboard.h"
#include "idt.h"
#include "port.h"
#include "vga.h"

/* Scancode Set 1 → ASCII lookup table (US QWERTY) */
static const char scancode_ascii[] = {
    0,   27,  '1', '2', '3', '4', '5', '6', '7', '8',  /* 0x00-0x09 */
    '9', '0', '-', '=', '\b', '\t',                      /* 0x0A-0x0F */
    'q', 'w', 'e', 'r', 't', 'y', 'u', 'i', 'o', 'p',  /* 0x10-0x19 */
    '[', ']', '\n', 0,                                     /* 0x1A-0x1D (0x1D=ctrl) */
    'a', 's', 'd', 'f', 'g', 'h', 'j', 'k', 'l',       /* 0x1E-0x26 */
    ';', '\'', '`', 0, '\\',                              /* 0x27-0x2B (0x2A=lshift) */
    'z', 'x', 'c', 'v', 'b', 'n', 'm',                  /* 0x2C-0x32 */
    ',', '.', '/', 0,                                      /* 0x33-0x36 (0x36=rshift) */
    '*', 0, ' '                                            /* 0x37=numpad*, 0x38=alt, 0x39=space */
};

/* Shifted variants */
static const char scancode_ascii_shift[] = {
    0,   27,  '!', '@', '#', '$', '%', '^', '&', '*',
    '(', ')', '_', '+', '\b', '\t',
    'Q', 'W', 'E', 'R', 'T', 'Y', 'U', 'I', 'O', 'P',
    '{', '}', '\n', 0,
    'A', 'S', 'D', 'F', 'G', 'H', 'J', 'K', 'L',
    ':', '"', '~', 0, '|',
    'Z', 'X', 'C', 'V', 'B', 'N', 'M',
    '<', '>', '?', 0,
    '*', 0, ' '
};

/* Key buffer */
static char key_buffer[KEY_BUFFER_SIZE];
static volatile int buffer_head = 0;
static volatile int buffer_tail = 0;

/* Modifier state */
static bool shift_pressed = false;
static bool caps_lock = false;
static bool ctrl_pressed = false;
static bool e0_prefix = false;  /* Extended scancode prefix (0xE0) */

/* --------------------------------------------------------------------------
 * buffer_put: Add a character to the circular key buffer
 * -------------------------------------------------------------------------- */
static void buffer_put(char c) {
    int next = (buffer_head + 1) % KEY_BUFFER_SIZE;
    if (next != buffer_tail) {
        key_buffer[buffer_head] = c;
        buffer_head = next;
    }
}

/* --------------------------------------------------------------------------
 * keyboard_callback: IRQ1 interrupt handler
 * -------------------------------------------------------------------------- */
static void keyboard_callback(struct registers* regs) {
    (void)regs;
    
    uint8_t scancode = port_byte_in(KEYBOARD_DATA_PORT);

    /* Extended scancode prefix — set flag and wait for next byte */
    if (scancode == 0xE0) {
        e0_prefix = true;
        return;
    }

    /* Handle extended (E0-prefixed) scancodes */
    if (e0_prefix) {
        e0_prefix = false;

        /* Ignore extended key releases */
        if (scancode & 0x80) return;

        /* Extended key presses: arrow keys */
        switch (scancode) {
            case KEY_UP:    buffer_put((char)0x80); return;
            case KEY_DOWN:  buffer_put((char)0x81); return;
            case KEY_LEFT:  buffer_put((char)0x82); return;
            case KEY_RIGHT: buffer_put((char)0x83); return;
            default: return;  /* Ignore other extended keys */
        }
    }

    /* Key release (bit 7 set) */
    if (scancode & 0x80) {
        uint8_t released = scancode & 0x7F;
        if (released == KEY_LSHIFT || released == KEY_RSHIFT) {
            shift_pressed = false;
        }
        if (released == KEY_LCTRL) {
            ctrl_pressed = false;
        }
        return;
    }

    /* Key press */
    switch (scancode) {
        case KEY_LSHIFT:
        case KEY_RSHIFT:
            shift_pressed = true;
            return;

        case KEY_CAPS_LOCK:
            caps_lock = !caps_lock;
            return;

        case KEY_LCTRL:
            ctrl_pressed = true;
            return;

        case KEY_LALT:
            return;

        default:
            break;
    }

    /* Ctrl+key combos (Ctrl+A=1, Ctrl+Q=17, Ctrl+S=19, etc.) */
    if (ctrl_pressed && scancode < sizeof(scancode_ascii)) {
        char c = scancode_ascii[scancode];
        if (c >= 'a' && c <= 'z') {
            buffer_put(c - 'a' + 1);  /* Ctrl+A=1, Ctrl+B=2, ... */
            return;
        }
    }

    /* Function keys F1-F4 (scancodes 0x3B-0x3E) */
    if (scancode >= 0x3B && scancode <= 0x3E) {
        buffer_put((char)scancode);  /* Pass raw scancode */
        return;
    }

    /* Convert scancode to ASCII */
    if (scancode < sizeof(scancode_ascii)) {
        char c;

        if (shift_pressed) {
            c = scancode_ascii_shift[scancode];
        } else {
            c = scancode_ascii[scancode];
        }

        /* Apply caps lock (only affects letters) */
        if (caps_lock && !shift_pressed && c >= 'a' && c <= 'z') {
            c -= 32;
        } else if (caps_lock && shift_pressed && c >= 'A' && c <= 'Z') {
            c += 32;
        }

        if (c != 0) {
            buffer_put(c);
        }
    }
}

/* --------------------------------------------------------------------------
 * keyboard_init: Register IRQ1 handler
 * -------------------------------------------------------------------------- */
void keyboard_init(void) {
    register_interrupt_handler(33, keyboard_callback);

    vga_print_color("[OK] ", VGA_COLOR(VGA_LIGHT_GREEN, VGA_BLACK));
    vga_print("Keyboard driver initialized (PS/2, IRQ1)\n");
}

/* --------------------------------------------------------------------------
 * keyboard_has_key: Check if a key is available
 * -------------------------------------------------------------------------- */
bool keyboard_has_key(void) {
    return buffer_head != buffer_tail;
}

/* --------------------------------------------------------------------------
 * keyboard_getchar: Get next character (blocks until available)
 * -------------------------------------------------------------------------- */
char keyboard_getchar(void) {
    while (!keyboard_has_key()) {
        __asm__ volatile("hlt");  /* Wait for interrupt */
    }
    char c = key_buffer[buffer_tail];
    buffer_tail = (buffer_tail + 1) % KEY_BUFFER_SIZE;
    return c;
}

/* --------------------------------------------------------------------------
 * keyboard_readline: Read a line with echo and backspace support
 * -------------------------------------------------------------------------- */
int keyboard_readline(char* buffer, int max_len) {
    int i = 0;
    while (i < max_len - 1) {
        char c = keyboard_getchar();

        if (c == '\n') {
            buffer[i] = '\0';
            vga_putchar('\n');
            return i;
        } else if (c == '\b') {
            if (i > 0) {
                i--;
                vga_backspace();
            }
        } else {
            buffer[i++] = c;
            vga_putchar(c);
        }
    }
    buffer[i] = '\0';
    return i;
}
