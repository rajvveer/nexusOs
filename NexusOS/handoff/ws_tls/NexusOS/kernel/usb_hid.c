/* ============================================================================
 * NexusOS — USB HID Driver (Implementation) — Phase 25 / 54 / 55
 * ============================================================================
 * USB HID boot-protocol driver. For each enumerated HID device it selects the
 * boot protocol, locates the interrupt-IN endpoint, and (from the idle pumps
 * — never an IRQ) polls that endpoint, decoding boot keyboard and mouse
 * reports. Keys are injected through keyboard_inject_char() with the same
 * KEYC_* codes the PS/2 driver produces (F1..F12, arrows, Delete, Spotlight,
 * Ctrl+letter, Ctrl+arrow snapping, Ctrl+F5..F8 desktops), so a USB keyboard
 * can do everything a PS/2 one can — including F12 to leave the desktop.
 * Phase 55: hot-plug aware (usb_hid_rescan binds/unbinds on device changes),
 * typematic repeat for held keys, report counters for `usbinfo`.
 * ============================================================================ */

#include "usb_hid.h"
#include "usb.h"
#include "vga.h"
#include "string.h"
#include "keyboard.h"
#include "mouse.h"

extern volatile uint32_t system_ticks;

#define HID_MAX 6

typedef struct {
    usb_device_t*   dev;       /* NULL = slot free */
    usb_endpoint_t* ep;        /* interrupt IN */
    uint32_t        gen;       /* dev->gen at bind time */
    int             is_kbd;    /* else mouse */
    uint8_t         last_mods;
    uint8_t         last_keys[6];
    uint8_t         rep_usage; /* key being auto-repeated (0 = none) */
    uint8_t         rep_mods;
    uint32_t        rep_tick;
} hid_dev_t;

static hid_dev_t hids[HID_MAX];
static bool      has_keyboard = false;
static bool      has_mouse = false;
static bool      caps_lock = false;
static uint32_t  total_reports = 0;

/* Mouse accumulated absolute position. */
static int mouse_px = 200, mouse_py = 200;

/* HID boot-keyboard usage -> ASCII (unshifted / shifted), 0 = ignore. */
static char hid_unshift(uint8_t u) {
    if (u >= 0x04 && u <= 0x1D) return 'a' + (u - 0x04);         /* a-z */
    if (u >= 0x1E && u <= 0x26) return '1' + (u - 0x1E);         /* 1-9 */
    if (u == 0x27) return '0';
    switch (u) {
        case 0x28: return '\n';   /* enter */
        case 0x2A: return '\b';   /* backspace */
        case 0x2B: return '\t';   /* tab */
        case 0x2C: return ' ';    /* space */
        case 0x2D: return '-';
        case 0x2E: return '=';
        case 0x2F: return '[';
        case 0x30: return ']';
        case 0x31: return '\\';
        case 0x33: return ';';
        case 0x34: return '\'';
        case 0x35: return '`';
        case 0x36: return ',';
        case 0x37: return '.';
        case 0x38: return '/';
        case 0x58: return '\n';   /* keypad enter */
        case 0x54: return '/';    /* keypad / */
        case 0x55: return '*';
        case 0x56: return '-';
        case 0x57: return '+';
        case 0x63: return '.';
    }
    if (u >= 0x59 && u <= 0x61) return '1' + (u - 0x59);         /* keypad 1-9 */
    if (u == 0x62) return '0';
    return 0;
}

static char hid_shift(uint8_t u) {
    if (u >= 0x04 && u <= 0x1D) return 'A' + (u - 0x04);
    switch (u) {
        case 0x1E: return '!';  case 0x1F: return '@';  case 0x20: return '#';
        case 0x21: return '$';  case 0x22: return '%';  case 0x23: return '^';
        case 0x24: return '&';  case 0x25: return '*';  case 0x26: return '(';
        case 0x27: return ')';
        case 0x2D: return '_';  case 0x2E: return '+';  case 0x2F: return '{';
        case 0x30: return '}';  case 0x31: return '|';  case 0x33: return ':';
        case 0x34: return '"';  case 0x35: return '~';  case 0x36: return '<';
        case 0x37: return '>';  case 0x38: return '?';
        case 0x28: return '\n'; case 0x2A: return '\b'; case 0x2B: return '\t';
        case 0x2C: return ' ';
    }
    if (u >= 0x54 && u <= 0x63) return hid_unshift(u);   /* keypad keys ignore shift */
    return 0;
}

/* Non-ASCII keys -> KEYC_* codes (same values the PS/2 driver emits). */
static int hid_special(uint8_t u, int ctrl) {
    if (u >= 0x3A && u <= 0x45) {                    /* F1..F12 */
        int n = u - 0x3A + 1;
        if (ctrl && n >= 5 && n <= 8) return KEYC_DESKTOP(n - 5);
        return KEYC_F(n);
    }
    switch (u) {
        case 0x4F: return ctrl ? KEYC_SNAP_RIGHT : KEYC_RIGHT;
        case 0x50: return ctrl ? KEYC_SNAP_LEFT  : KEYC_LEFT;
        case 0x51: return ctrl ? KEYC_RESTORE    : KEYC_DOWN;
        case 0x52: return ctrl ? KEYC_MAXIMIZE   : KEYC_UP;
        case 0x4C: return KEYC_DELETE;
        case 0x29: return 27;                        /* Escape */
        /* Home/End/PgUp/PgDn have no PS/2 translation in this kernel. */
        default:   return 0;
    }
}

/* Translate one newly pressed usage under the given modifier byte. */
static int hid_translate(uint8_t u, uint8_t mods) {
    int ctrl  = (mods & 0x11) != 0;                  /* L/R Ctrl */
    int shift = (mods & 0x22) != 0;                  /* L/R Shift */
    int gui   = (mods & 0x88) != 0;                  /* L/R GUI (Win) */

    if (u == 0x39) { caps_lock = !caps_lock; return 0; }       /* Caps Lock */
    if ((ctrl || gui) && u == 0x2C) return KEYC_SPOTLIGHT;      /* Ctrl/Win+Space */
    int sp = hid_special(u, ctrl);
    if (sp) return sp;
    if (ctrl) {                                      /* Ctrl+A=1 ... Ctrl+Z=26 */
        char c = hid_unshift(u);
        if (c >= 'a' && c <= 'z') return c - 'a' + 1;
        return 0;
    }
    char c = shift ? hid_shift(u) : hid_unshift(u);
    if (caps_lock && c) {
        if (!shift && c >= 'a' && c <= 'z') c -= 32;
        else if (shift && c >= 'A' && c <= 'Z') c += 32;
    }
    return c;
}

static void hid_log_bind(hid_dev_t* h) {
    char path[24];
    usb_port_path(h->dev, path, sizeof(path));
    usb_log("USB-HID: bound "); usb_log(h->is_kbd ? "keyboard" : "mouse");
    usb_log(" at "); usb_log(path); usb_log("\n");
}

/* --------------------------------------------------------------------------
 * usb_hid_rescan: drop bindings of vanished devices, bind new HID devices.
 * Called at init and by the USB core after any hot-plug change.
 * -------------------------------------------------------------------------- */
void usb_hid_rescan(void) {
    for (int i = 0; i < HID_MAX; i++) {
        hid_dev_t* h = &hids[i];
        if (h->dev && (!h->dev->active || h->dev->gen != h->gen)) {
            usb_log("USB-HID: unbound "); usb_log(h->is_kbd ? "keyboard\n" : "mouse\n");
            memset(h, 0, sizeof(*h));
        }
    }

    usb_device_t* devs = usb_get_devices();
    int count = usb_device_count();
    for (int i = 0; i < count; i++) {
        usb_device_t* d = &devs[i];
        if (!d->active || !d->configured || d->class_code != USB_CLASS_HID) continue;
        int bound = 0;
        for (int k = 0; k < HID_MAX; k++)
            if (hids[k].dev == d && hids[k].gen == d->gen) { bound = 1; break; }
        if (bound) continue;

        usb_endpoint_t* ep = usb_find_endpoint(d, USB_EP_XFER_INT, 1 /*IN*/);
        if (!ep) continue;
        hid_dev_t* h = NULL;
        for (int k = 0; k < HID_MAX; k++) if (!hids[k].dev) { h = &hids[k]; break; }
        if (!h) break;

        /* SET_PROTOCOL(boot=0) + SET_IDLE(0): report only on change. Harmless
         * if the device is already in boot mode; a STALL is tolerated. */
        usb_control(d, USB_DIR_OUT | USB_TYPE_CLASS | USB_RECIP_INTERFACE,
                    USB_HID_SET_PROTOCOL, 0, 0, 0, 0);
        usb_control(d, USB_DIR_OUT | USB_TYPE_CLASS | USB_RECIP_INTERFACE,
                    USB_HID_SET_IDLE, 0, 0, 0, 0);

        memset(h, 0, sizeof(*h));
        h->dev = d;
        h->ep = ep;
        h->gen = d->gen;
        h->is_kbd = (d->protocol != USB_HID_PROTO_MOUSE);   /* default: keyboard */
        hid_log_bind(h);
    }

    has_keyboard = false;
    has_mouse = false;
    for (int i = 0; i < HID_MAX; i++) {
        if (!hids[i].dev) continue;
        if (hids[i].is_kbd) has_keyboard = true; else has_mouse = true;
    }
}

/* --------------------------------------------------------------------------
 * usb_hid_init: bind every enumerated HID device.
 * -------------------------------------------------------------------------- */
void usb_hid_init(void) {
    memset(hids, 0, sizeof(hids));
    has_keyboard = false;
    has_mouse = false;
    caps_lock = false;
    total_reports = 0;
    usb_hid_rescan();

    if (has_keyboard || has_mouse) {
        vga_print_color("[OK] ", VGA_COLOR(VGA_LIGHT_GREEN, VGA_BLACK));
        vga_print("USB HID: ");
        if (has_keyboard) vga_print("keyboard ");
        if (has_mouse) vga_print("mouse ");
        vga_print("(boot protocol)\n");
    }
}

/* --------------------------------------------------------------------------
 * decode_keyboard: emit newly-pressed keys from an 8-byte boot report.
 * -------------------------------------------------------------------------- */
static void decode_keyboard(hid_dev_t* h, uint8_t* r) {
    uint8_t mods = r[0];
    if (r[2] == 0x01) return;                        /* ErrorRollOver phantom state */

    for (int i = 2; i < 8; i++) {
        uint8_t u = r[i];
        if (u == 0) continue;
        int held = 0;
        for (int j = 0; j < 6; j++) if (h->last_keys[j] == u) { held = 1; break; }
        if (held) continue;
        int code = hid_translate(u, mods);
        if (code) keyboard_inject_char((char)code);
        if (u != 0x39) {                             /* start typematic repeat */
            h->rep_usage = u;
            h->rep_mods = mods;
            h->rep_tick = system_ticks;
        }
    }
    if (h->rep_usage) {                              /* repeating key released? */
        int still = 0;
        for (int i = 2; i < 8; i++) if (r[i] == h->rep_usage) { still = 1; break; }
        if (!still) h->rep_usage = 0;
        else h->rep_mods = mods;
    }
    for (int i = 0; i < 6; i++) h->last_keys[i] = r[i + 2];
    h->last_mods = mods;
}

/* --------------------------------------------------------------------------
 * decode_mouse: accumulate motion and inject an absolute pointer event.
 * -------------------------------------------------------------------------- */
static void decode_mouse(uint8_t* r) {
    int8_t dx = (int8_t)r[1];
    int8_t dy = (int8_t)r[2];
    mouse_px += dx;
    mouse_py += dy;
    if (mouse_px < 0) mouse_px = 0;
    if (mouse_py < 0) mouse_py = 0;
    mouse_inject(mouse_px, mouse_py, r[0] & 0x07);
}

/* --------------------------------------------------------------------------
 * usb_hid_poll: pump the USB core (hot-plug) and poll each HID endpoint once.
 * Called from the shell idle pump, the desktop loop and the login loop (task
 * context), so it may run control/interrupt transfers.
 * -------------------------------------------------------------------------- */
void usb_hid_poll(void) {
    usb_poll();                                      /* Phase 55: root-port + hub hot-plug */

    uint8_t rpt[8];
    for (int i = 0; i < HID_MAX; i++) {
        hid_dev_t* h = &hids[i];
        if (!h->dev) continue;
        if (!h->dev->active || h->dev->gen != h->gen) continue;   /* rescan will drop it */
        memset(rpt, 0, sizeof(rpt));
        int n = usb_interrupt_in(h->dev, h->ep, rpt, 8);
        if (n > 0) {
            total_reports++;
            if (h->is_kbd) decode_keyboard(h, rpt);
            else decode_mouse(rpt);
        }
        /* Typematic repeat: 500 ms delay, then one character per PIT tick. */
        if (h->is_kbd && h->rep_usage && (uint32_t)(system_ticks - h->rep_tick) >= 9) {
            int code = hid_translate(h->rep_usage, h->rep_mods);
            if (code) keyboard_inject_char((char)code);
            h->rep_tick = system_ticks - 8;
        }
    }
}

bool     usb_hid_keyboard_present(void) { return has_keyboard; }
bool     usb_hid_mouse_present(void)    { return has_mouse; }
uint32_t usb_hid_report_count(void)     { return total_reports; }
