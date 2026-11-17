/* ============================================================================
 * NexusOS — VNC / RFB Remote Desktop Server — Phase 45
 * ============================================================================
 * See vnc.h for the design. Mirrors rshell.c's listener structure: one
 * tcp_listen() TCB slot, a poll() that drains conn->rx_buf each call and
 * re-listens after a client drops.
 *
 * INTEGER MATH ONLY — no 64-bit multiply/divide (no libgcc in this
 * freestanding kernel). The served size is the real framebuffer, capped at
 * VNC_MAX_W x VNC_MAX_H, so the largest product is the pixel offset
 * y*stride (< 2048*65535, fits int) and a band is <= BAND_BYTES (fits
 * uint16_t). Never compute a whole-frame byte total.
 * ============================================================================ */

#include "vnc.h"
#include "tcp.h"
#include "ip.h"
#include "net.h"
#include "framebuffer.h"
#include "keyboard.h"
#include "mouse.h"
#include "mobile.h"
#include "clipboard.h"
#include "vga.h"
#include "string.h"
/* Phase 55: RFB VNC Authentication */
#include "des.h"
#include "vfs.h"
#include "ramfs.h"
#include "users.h"
#include "rtc.h"
#include "syslog.h"

extern volatile uint32_t system_ticks;

/* --------------------------------------------------------------------------
 * Geometry / tiling
 * -------------------------------------------------------------------------- */
#define VNC_MAX_W   2048                     /* served-size caps (tile table)  */
#define VNC_MAX_H   2048
#define TILE        64
#define TILE_MAX    ((VNC_MAX_W / TILE) * (VNC_MAX_H / TILE))   /* 1024 */
#define BAND_BYTES  32768                    /* bytes per tcp_send band        */
#define POLL_BYTES  262144                   /* per-poll send cap (~256 KB)    */

/* Served geometry, read from the framebuffer each session (reset_session):
 * vw x vh on the wire; stride = back-buffer row length in pixels. */
static int vw = 0, vh = 0, stride = 0, tiles_x = 0, tile_count = 0;

/* --------------------------------------------------------------------------
 * RFB handshake phase
 * -------------------------------------------------------------------------- */
typedef enum {
    RFB_WAIT_VERSION,     /* sent our version, waiting for the client's      */
    RFB_WAIT_SECTYPE,     /* 3.7/3.8: sent the type list, waiting for choice */
    RFB_WAIT_AUTH,        /* sent the 16-byte challenge, waiting for response */
    RFB_WAIT_CLIENTINIT,
    RFB_READY
} rfb_phase_t;

/* --------------------------------------------------------------------------
 * Server state
 * -------------------------------------------------------------------------- */
static bool        vnc_running = false;
static int         vnc_conn_idx = -1;
static uint32_t    vnc_conn_gen = 0;    /* Phase 55: TCB slot-reuse guard */
static rfb_phase_t phase = RFB_WAIT_VERSION;
static bool        version_sent = false;

static uint8_t     pbuf[300];     /* protocol parse accumulator              */
static int         plen = 0;
static uint32_t    skip_bytes = 0;/* remainder of an oversized message       */

static bool        req_pending = false;
static bool        req_incremental = false;

static uint32_t    tile_hash[TILE_MAX];
static int         hash_cursor = 0;
static int         dirty_y0 = 0, dirty_y1 = 0;   /* dirty row span [y0, y1)   */

static const char* last_clip = NULL;
static uint32_t    frames_sent = 0;
static bool        client_ready = false;

/* ---- Phase 55: authentication state ---- */
#define VNC_PASSWD_PATH   "/etc/vncpasswd"
#define VNC_LOCKOUT_TICKS 54              /* 3 s at 18.2 Hz                 */
static bool        vnc_insecure = false;  /* started with --insecure        */
static int         client_minor = 3;      /* negotiated RFB 3.x minor        */
static uint8_t     challenge[16];
static uint32_t    lockout_until = 0;     /* no new handshake before this    */
static uint32_t    auth_failures = 0;
static uint32_t    rng_state = 0x9E3779B9u;
static uint32_t    rng_counter = 0;

/* Classic VNC password-file key {23,82,107,6,35,78,88,7} as d3des consumes
 * it (LSB-first), i.e. bit-reversed per byte for a standard DES. Using the
 * real format means /etc/vncpasswd is byte-compatible with ~/.vnc/passwd. */
static const uint8_t vnc_obf_key[8] = { 0xE8,0x4A,0xD6,0x60,0xC4,0x72,0x1A,0xE0 };

/* --------------------------------------------------------------------------
 * Connection helpers (re-fetch the TCB each poll, like rshell)
 * -------------------------------------------------------------------------- */
static tcp_conn_t* get_conn(void) {
    if (vnc_conn_idx < 0) return NULL;
    int count;
    const tcp_conn_t* all = tcp_get_connections(&count);
    if (vnc_conn_idx >= count) return NULL;
    tcp_conn_t* c = (tcp_conn_t*)&all[vnc_conn_idx];
    if (!c->active) return NULL;
    if (c->gen != vnc_conn_gen) return NULL;    /* slot was recycled for someone else */
    return c;
}

static int send_raw(tcp_conn_t* c, const void* data, uint16_t len) {
    if (!c || !c->connected) return -1;
    return tcp_send(c, data, len);
}

/* Big-endian packers (RFB integers are network byte order on the wire). */
static void put16(uint8_t* p, uint16_t v) { p[0] = (uint8_t)(v >> 8); p[1] = (uint8_t)v; }
static void put32(uint8_t* p, uint32_t v) {
    p[0] = (uint8_t)(v >> 24); p[1] = (uint8_t)(v >> 16);
    p[2] = (uint8_t)(v >> 8);  p[3] = (uint8_t)v;
}
static uint16_t get16(const uint8_t* p) { return (uint16_t)((p[0] << 8) | p[1]); }
static uint32_t get32(const uint8_t* p) {
    return ((uint32_t)p[0] << 24) | ((uint32_t)p[1] << 16) |
           ((uint32_t)p[2] << 8)  | (uint32_t)p[3];
}

/* Drop n bytes from the front of pbuf. */
static void pbuf_drop(int n) {
    if (n >= plen) { plen = 0; return; }
    memcpy(pbuf, pbuf + n, plen - n);
    plen -= n;
}

/* Mark the whole screen dirty (full repaint on next request). */
static void mark_all_dirty(void) {
    for (int i = 0; i < tile_count; i++) tile_hash[i] = 0xFFFFFFFFu;
    dirty_y0 = 0; dirty_y1 = vh;
}

/* Pick up the real framebuffer size (0x0 in text mode -> nothing to serve). */
static void vnc_geom(void) {
    stride = fb_is_vesa() ? (int)fb_get_width() : 0;
    vh = stride ? (int)fb_get_height() : 0;
    vw = stride < VNC_MAX_W ? stride : VNC_MAX_W;
    if (vh > VNC_MAX_H) vh = VNC_MAX_H;
    tiles_x = (vw + TILE - 1) / TILE;
    tile_count = tiles_x * ((vh + TILE - 1) / TILE);
}

/* --------------------------------------------------------------------------
 * ServerInit message (sent once after ClientInit)
 * -------------------------------------------------------------------------- */
static void send_server_init(tcp_conn_t* c) {
    uint8_t m[24 + 7];
    int p = 0;
    put16(m + p, (uint16_t)vw); p += 2;    /* framebuffer-width  */
    put16(m + p, (uint16_t)vh); p += 2;    /* framebuffer-height */
    /* PIXEL_FORMAT (16 bytes) — matches 0x00RRGGBB exactly */
    m[p++] = 32;        /* bits-per-pixel        */
    m[p++] = 24;        /* depth                 */
    m[p++] = 0;         /* big-endian-flag = 0   */
    m[p++] = 1;         /* true-colour-flag = 1  */
    put16(m + p, 255); p += 2;   /* red-max   */
    put16(m + p, 255); p += 2;   /* green-max */
    put16(m + p, 255); p += 2;   /* blue-max  */
    m[p++] = 16;        /* red-shift   */
    m[p++] = 8;         /* green-shift */
    m[p++] = 0;         /* blue-shift  */
    m[p++] = 0; m[p++] = 0; m[p++] = 0;  /* padding */
    /* name */
    put32(m + p, 7); p += 4;             /* name-length */
    memcpy(m + p, "NexusOS", 7); p += 7;
    send_raw(c, m, (uint16_t)p);
}

/* ==========================================================================
 * Phase 55: RFB VNC Authentication (security type 2)
 * ========================================================================== */

/* ---- password store: /etc/vncpasswd = DES(obf_key, password[8]) ---- */
static fs_node_t* vnc_passwd_node(void) {
    fs_node_t* n = ramfs_resolve(VNC_PASSWD_PATH, NULL);
    if (!n || n->type != FS_FILE || n->size < 8) return NULL;
    return n;
}

bool vnc_has_password(void) { return vnc_passwd_node() != NULL; }

/* Load the plaintext password (8 bytes, zero padded). Reads through the
 * node's own read op so a non-root console session (0600 file) can still
 * authenticate remote clients. */
static bool vnc_load_password(uint8_t out[8]) {
    fs_node_t* n = vnc_passwd_node();
    if (!n || !n->read) return false;
    uint8_t obf[8];
    if (n->read(n, 0, 8, obf) != 8) return false;
    des_ctx_t c;
    des_set_key(&c, vnc_obf_key);
    des_decrypt_block(&c, obf, out);
    return true;
}

int vnc_set_password(const char* password) {
    if (!password || !password[0]) return -1;
    uint8_t plain[8], obf[8];
    memset(plain, 0, 8);
    for (int i = 0; i < 8 && password[i]; i++) plain[i] = (uint8_t)password[i];
    des_ctx_t c;
    des_set_key(&c, vnc_obf_key);
    des_encrypt_block(&c, plain, obf);

    fs_node_t* etc = ramfs_resolve("/etc", NULL);
    if (!etc) { ramfs_mkdir("/etc", NULL); etc = ramfs_resolve("/etc", NULL); }
    if (!etc || etc->type != FS_DIRECTORY) return -2;
    fs_node_t* n = vfs_finddir(etc, "vncpasswd");
    if (n && n->type != FS_FILE) return -2;
    if (!n) n = ramfs_mknode(etc, "vncpasswd", FS_FILE);
    if (!n || !n->write) return -2;
    n->uid = UID_ROOT; n->gid = UID_ROOT; n->mode = 0600;   /* root-only secret */
    n->size = 0;                                            /* truncate */
    if (n->write(n, 0, 8, obf) != 8) return -2;
    return 0;
}

bool vnc_clear_password(void) {
    return ramfs_rm_path(VNC_PASSWD_PATH, NULL) == 0;
}

uint32_t vnc_auth_failures(void) { return auth_failures; }

/* ---- challenge: 16 bytes that differ per connection ----
 * Entropy: TSC (rdtsc), PIT ticks, RTC, a counter; stirred by xorshift32 and
 * an FNV-style multiply. Not a CSPRNG, but unpredictable enough that two
 * sessions never see the same challenge (the test asserts that). */
static uint32_t rng_next(void) {
    uint32_t x = rng_state;
    x ^= x << 13; x ^= x >> 17; x ^= x << 5;
    rng_state = x ? x : 0x9E3779B9u;
    return x * 2654435761u;
}

static void make_challenge(uint8_t out[16]) {
    uint32_t lo, hi;
    __asm__ volatile("rdtsc" : "=a"(lo), "=d"(hi));
    rtc_time_t t; rtc_read(&t);
    uint32_t rtc = ((uint32_t)t.second << 24) | ((uint32_t)t.minute << 16) |
                   ((uint32_t)t.hour << 8) | t.day;
    rng_counter++;
    rng_state ^= lo ^ (hi << 7) ^ (system_ticks * 40503u) ^ (rng_counter * 2654435761u) ^ rtc;
    if (!rng_state) rng_state = 0x9E3779B9u;
    for (int i = 0; i < 16; i += 4) {
        uint32_t r = rng_next() ^ (lo + (uint32_t)i * 16777619u);
        out[i]     = (uint8_t)(r >> 24); out[i + 1] = (uint8_t)(r >> 16);
        out[i + 2] = (uint8_t)(r >> 8);  out[i + 3] = (uint8_t)r;
    }
}

/* DES(challenge) with key = password bytes bit-reversed (VNC quirk), compared
 * against the 16-byte client response with a constant-shape loop. */
static bool vnc_check_response(const uint8_t* resp) {
    uint8_t pw[8], key[8], want[16];
    if (!vnc_load_password(pw)) return false;
    for (int i = 0; i < 8; i++) key[i] = des_bitrev8(pw[i]);
    des_ctx_t c;
    des_set_key(&c, key);
    des_encrypt_block(&c, challenge, want);
    des_encrypt_block(&c, challenge + 8, want + 8);
    uint8_t diff = 0;
    for (int i = 0; i < 16; i++) diff |= (uint8_t)(want[i] ^ resp[i]);
    memset(pw, 0, 8); memset(key, 0, 8);
    return diff == 0;
}

/* SecurityResult: u32 0 = OK, 1 = failed (+ reason string for RFB 3.8). */
static void send_security_result(tcp_conn_t* c, bool ok, const char* reason) {
    uint8_t m[4 + 4 + 64];
    put32(m, ok ? 0u : 1u);
    int n = 4;
    if (!ok && client_minor >= 8 && reason) {
        uint32_t rl = (uint32_t)strlen(reason);
        if (rl > 64) rl = 64;
        put32(m + 4, rl);
        memcpy(m + 8, reason, rl);
        n = 8 + (int)rl;
    }
    send_raw(c, m, (uint16_t)n);
}

/* Begin the challenge/response: send 16 random bytes. */
static void start_challenge(tcp_conn_t* c) {
    make_challenge(challenge);
    send_raw(c, challenge, 16);
    phase = RFB_WAIT_AUTH;
}

/* ---- shell: vncpasswd ---- */
void vnc_cmd_passwd(int argc, char* argv[]) {
    if (!users_current_is_root()) {
        vga_print_color("  vncpasswd: root only\n", VGA_COLOR(VGA_LIGHT_RED, VGA_BLACK));
        return;
    }
    if (argc < 2) {
        vga_print("  Usage: vncpasswd <password>   (1..8 chars used, like real VNC)\n");
        vga_print("         vncpasswd clear        (remove /etc/vncpasswd)\n");
        vga_print("  VNC password is ");
        vga_print(vnc_has_password() ? "SET (" VNC_PASSWD_PATH ")\n" : "not set\n");
        return;
    }
    if (strcmp(argv[1], "clear") == 0) {
        if (vnc_clear_password())
            vga_print_color("  VNC password cleared (" VNC_PASSWD_PATH " removed)\n", VGA_COLOR(VGA_YELLOW, VGA_BLACK));
        else
            vga_print("  No VNC password was set\n");
        return;
    }
    int rc = vnc_set_password(argv[1]);
    if (rc == 0) {
        vga_print_color("  VNC password set -> " VNC_PASSWD_PATH " (mode 0600, root)", VGA_COLOR(VGA_LIGHT_GREEN, VGA_BLACK));
        if (strlen(argv[1]) > 8) vga_print_color("  [truncated to 8 chars]", VGA_COLOR(VGA_YELLOW, VGA_BLACK));
        vga_print("\n");
    } else {
        vga_print_color("  vncpasswd: failed to store the password\n", VGA_COLOR(VGA_LIGHT_RED, VGA_BLACK));
    }
}

/* --------------------------------------------------------------------------
 * Input injection
 * -------------------------------------------------------------------------- */
static void inject_key(uint32_t keysym) {
    char ch = 0;
    if (keysym >= 0x20 && keysym <= 0x7E) ch = (char)keysym;
    else switch (keysym) {
        case 0xFF0D: ch = '\n'; break;   /* Return    */
        case 0xFF08: ch = 0x08; break;   /* Backspace */
        case 0xFF09: ch = '\t'; break;   /* Tab       */
        case 0xFF1B: ch = 0x1B; break;   /* Escape    */
        case 0xFF52: ch = (char)0x80; break;  /* Up    */
        case 0xFF54: ch = (char)0x81; break;  /* Down  */
        case 0xFF51: ch = (char)0x82; break;  /* Left  */
        case 0xFF53: ch = (char)0x83; break;  /* Right */
        default: return;
    }
    keyboard_inject_char(ch);
}

static void inject_pointer(uint16_t x, uint16_t y, uint8_t rfb_mask) {
    /* RFB: bit0=L, bit1=M, bit2=R.  mouse.h: L=0x01, R=0x02, M=0x04. */
    uint8_t m = 0;
    if (rfb_mask & 0x01) m |= MOUSE_LEFT;
    if (rfb_mask & 0x02) m |= MOUSE_MIDDLE;
    if (rfb_mask & 0x04) m |= MOUSE_RIGHT;
    mouse_inject((int)x, (int)y, m);
    /* Phase 47: sample the gesture recognizer on every injected pointer event
     * so a remote (VNC/touch) press->move->release is seen at full event rate,
     * not just once per idle loop. No-op unless touch mode is on. */
    mobile_poll();
}

/* --------------------------------------------------------------------------
 * Dirty detection: hash a rolling band of tiles (sub-sampled 4x4)
 * -------------------------------------------------------------------------- */
static void hash_tiles(void) {
    const uint32_t* back = fb_get_backbuffer();
    if (!back || tile_count <= 0) return;

    int per = (tile_count + 7) / 8;      /* whole screen every ~8 polls */
    for (int n = 0; n < per; n++) {
        int i  = (hash_cursor + n) % tile_count;
        int x0 = (i % tiles_x) * TILE, y0 = (i / tiles_x) * TILE;
        int x1 = x0 + TILE < vw ? x0 + TILE : vw;   /* edge tiles are clipped */
        int y1 = y0 + TILE < vh ? y0 + TILE : vh;

        uint32_t h = 2166136261u;
        for (int y = y0; y < y1; y += 4) {
            const uint32_t* row = back + y * stride;
            for (int x = x0; x < x1; x += 4) {
                h ^= row[x];
                h *= 16777619u;          /* 32-bit imul — no libgcc */
            }
        }
        if (h != tile_hash[i]) {
            tile_hash[i] = h;
            if (dirty_y1 <= dirty_y0) { dirty_y0 = y0; dirty_y1 = y1; }
            else { if (y0 < dirty_y0) dirty_y0 = y0; if (y1 > dirty_y1) dirty_y1 = y1; }
        }
    }
    hash_cursor = (hash_cursor + per) % tile_count;
}

/* --------------------------------------------------------------------------
 * Send one FramebufferUpdate (full-width dirty band, Raw)
 * -------------------------------------------------------------------------- */
static void send_update(tcp_conn_t* c) {
    if (dirty_y1 <= dirty_y0) return;  /* nothing changed */

    const uint32_t* back = fb_get_backbuffer();
    if (!back) return;

    int y = dirty_y0;
    int h = dirty_y1 - dirty_y0;
    if (y < 0) y = 0;
    if (y + h > vh) h = vh - y;
    if (h <= 0 || vw <= 0) { req_pending = false; dirty_y0 = dirty_y1 = 0; return; }

    /* Per-poll cap. Clamp h BEFORE the rect header so the declared height
     * always matches the pixel bytes sent; the rest stays dirty. Full-width
     * rows are contiguous only when vw == stride, else one row per send. */
    int row_bytes = vw * 4;                               /* <= 8192 */
    int band = (vw == stride) ? BAND_BYTES / row_bytes : 1;   /* >= 4 */
    int cap  = POLL_BYTES / row_bytes;                    /* >= 32 rows */
    int full = h;
    if (h > cap) h = cap;

    /* FramebufferUpdate header: type(1)=0, pad(1), num-rects(2)=1 */
    uint8_t hdr[4];
    hdr[0] = 0; hdr[1] = 0; put16(hdr + 2, 1);
    if (send_raw(c, hdr, 4) < 0) return;

    /* Rectangle header: x(2), y(2), w(2), h(2), encoding(4)=0 (Raw) */
    uint8_t rh[12];
    put16(rh + 0, 0);             /* x */
    put16(rh + 2, (uint16_t)y);   /* y */
    put16(rh + 4, (uint16_t)vw);  /* w */
    put16(rh + 6, (uint16_t)h);   /* h */
    put32(rh + 8, 0);             /* encoding = Raw */
    if (send_raw(c, rh, 12) < 0) return;

    /* Pixel data, banded: each band is a zero-copy slice of the back buffer,
     * band*row_bytes <= BAND_BYTES (uint16_t ok). */
    for (int row = 0; row < h; ) {
        int rows = h - row;
        if (rows > band) rows = band;
        if (send_raw(c, back + (y + row) * stride, (uint16_t)(rows * row_bytes)) < 0) return;
        row += rows;
    }

    frames_sent++;

    if (h >= full) {
        /* whole band sent — clear dirty state */
        req_pending = false;
        dirty_y0 = dirty_y1 = 0;
    } else {
        /* hit the per-poll cap — keep the rest dirty (req stays pending, so
         * the next poll sends it as a further, self-contained update) */
        dirty_y0 = y + h;
    }
}

/* --------------------------------------------------------------------------
 * Clipboard out: send ServerCutText when the local clipboard changed
 * -------------------------------------------------------------------------- */
static void clipboard_out(tcp_conn_t* c) {
    const char* cur = clipboard_paste();
    if (!cur || cur == last_clip) return;

    uint32_t len = (uint32_t)strlen(cur);
    if (len > 255) len = 255;

    uint8_t hdr[8];
    hdr[0] = 3; hdr[1] = 0; hdr[2] = 0; hdr[3] = 0;  /* type 3 + 3 pad */
    put32(hdr + 4, len);
    if (send_raw(c, hdr, 8) < 0) return;
    if (len > 0) send_raw(c, cur, (uint16_t)len);
    last_clip = cur;
}

/* --------------------------------------------------------------------------
 * Handle ClientCutText payload -> local clipboard
 * -------------------------------------------------------------------------- */
static void clipboard_in(const uint8_t* text, uint32_t tlen) {
    char tmp[256];
    if (tlen > 255) tlen = 255;
    memcpy(tmp, text, tlen);
    tmp[tlen] = '\0';
    clipboard_copy(tmp);
    last_clip = clipboard_paste();   /* avoid echoing it straight back out */
}

/* --------------------------------------------------------------------------
 * Parse buffered client messages (READY phase). Returns when no complete
 * message remains; partial messages stay buffered for the next poll.
 * -------------------------------------------------------------------------- */
static void parse_client(tcp_conn_t* c) {
    for (;;) {
        if (plen < 1) return;
        uint8_t type = pbuf[0];

        switch (type) {
            case 0:  /* SetPixelFormat: 1 + 3 pad + 16 */
                if (plen < 20) return;
                pbuf_drop(20);
                break;

            case 2: { /* SetEncodings: 1 + 1 pad + 2 count + 4*count */
                if (plen < 4) return;
                uint16_t n = get16(pbuf + 2);
                uint32_t full = 4u + 4u * (uint32_t)n;
                if (full > sizeof(pbuf)) {
                    /* too big to buffer — discard what we have, skip the rest */
                    skip_bytes = full - (uint32_t)plen;
                    plen = 0;
                    return;
                }
                if ((uint32_t)plen < full) return;
                pbuf_drop((int)full);
                break;
            }

            case 3:  /* FramebufferUpdateRequest: 1 + incr(1) + x,y,w,h (8) */
                if (plen < 10) return;
                req_incremental = pbuf[1] ? true : false;
                if (!req_incremental) mark_all_dirty();
                req_pending = true;
                pbuf_drop(10);
                break;

            case 4:  /* KeyEvent: 1 + down(1) + pad(2) + keysym(4) */
                if (plen < 8) return;
                if (pbuf[1]) inject_key(get32(pbuf + 4));
                pbuf_drop(8);
                break;

            case 5:  /* PointerEvent: 1 + button-mask(1) + x(2) + y(2) */
                if (plen < 6) return;
                inject_pointer(get16(pbuf + 2), get16(pbuf + 4), pbuf[1]);
                pbuf_drop(6);
                break;

            case 6: { /* ClientCutText: 1 + 3 pad + len(4) + text */
                if (plen < 8) return;
                uint32_t tlen = get32(pbuf + 4);
                /* Bound tlen itself: 8u + tlen wraps for a hostile length
                 * (e.g. 0xFFFFFFF8 -> 0), and pbuf_drop(0) spun forever. */
                if (tlen > sizeof(pbuf) - 8u) {
                    /* take what text we have, skip the remainder */
                    uint32_t have = (uint32_t)plen - 8u;   /* < tlen */
                    clipboard_in(pbuf + 8, have);
                    skip_bytes = tlen - have;
                    plen = 0;
                    return;
                }
                if ((uint32_t)plen < 8u + tlen) return;
                clipboard_in(pbuf + 8, tlen);
                pbuf_drop((int)(8u + tlen));
                break;
            }

            default:
                /* unknown — drop one byte to attempt resync */
                pbuf_drop(1);
                break;
        }
    }
    (void)c;
}

/* --------------------------------------------------------------------------
 * Drain the TCP RX window into pbuf, honoring an active skip_bytes counter.
 * -------------------------------------------------------------------------- */
static void drain_rx(tcp_conn_t* c) {
    if (c->rx_len == 0) return;
    int i = 0;

    /* The timer-IRQ net_poll appends to rx_buf/rx_len: copy out + clear with
     * interrupts off (save/restore EFLAGS); parsing happens later, outside. */
    uint32_t eflags;
    __asm__ volatile("pushf; pop %0; cli" : "=r"(eflags) :: "memory");

    /* First consume any pending skip (oversized-message remainder). */
    if (skip_bytes > 0) {
        uint32_t avail = c->rx_len;
        uint32_t take = (avail < skip_bytes) ? avail : skip_bytes;
        i += (int)take;
        skip_bytes -= take;
    }

    /* Buffer the rest (drop anything that would overflow pbuf — protocol
     * messages we care about are all < 300 bytes once skip handles the big
     * ones). */
    while (i < c->rx_len && plen < (int)sizeof(pbuf)) {
        pbuf[plen++] = c->rx_buf[i++];
    }
    c->rx_len = 0;   /* ALWAYS clear the window */
    __asm__ volatile("push %0; popf" :: "r"(eflags) : "memory", "cc");
}

/* --------------------------------------------------------------------------
 * Reset per-connection protocol state
 * -------------------------------------------------------------------------- */
static void reset_session(void) {
    vnc_geom();
    phase = RFB_WAIT_VERSION;
    version_sent = false;
    plen = 0;
    skip_bytes = 0;
    req_pending = false;
    req_incremental = false;
    hash_cursor = 0;
    dirty_y0 = dirty_y1 = 0;
    last_clip = NULL;
    client_ready = false;
    for (int i = 0; i < tile_count; i++) tile_hash[i] = 0;
}

/* --------------------------------------------------------------------------
 * Lifecycle
 * -------------------------------------------------------------------------- */
void vnc_init(void) {
    vnc_running = false;
    vnc_conn_idx = -1;
    frames_sent = 0;
    reset_session();

    vga_print_color("[OK] ", VGA_COLOR(VGA_LIGHT_GREEN, VGA_BLACK));
    vga_print("VNC server initialized (port 5900)\n");
}

bool vnc_start_mode(bool insecure) {
    if (vnc_running) {
        vga_print_color("  VNC: already running\n", VGA_COLOR(VGA_YELLOW, VGA_BLACK));
        return true;
    }
    /* Phase 55: authenticated by default. Without a VNC password there is
     * nothing to verify the DES response against, so refuse to listen. */
    if (!insecure && !vnc_has_password()) {
        vga_print_color("  VNC: no VNC password set - set one with vncpasswd first\n",
                        VGA_COLOR(VGA_LIGHT_RED, VGA_BLACK));
        vga_print_color("       (or 'vnc --insecure' for an unauthenticated server)\n",
                        VGA_COLOR(VGA_DARK_GREY, VGA_BLACK));
        return false;
    }
    tcp_conn_t* conn = tcp_listen(VNC_PORT);
    if (!conn) {
        vga_print_color("  VNC: No free TCP slots\n", VGA_COLOR(VGA_LIGHT_RED, VGA_BLACK));
        return false;
    }
    vnc_running = true;
    vnc_insecure = insecure;
    reset_session();

    int count;
    const tcp_conn_t* all = tcp_get_connections(&count);
    vnc_conn_idx = -1;
    for (int i = 0; i < count; i++)
        if (&all[i] == conn) { vnc_conn_idx = i; break; }
    vnc_conn_gen = conn->gen;

    vga_print_color("  VNC: Listening on port 5900\n", VGA_COLOR(VGA_LIGHT_GREEN, VGA_BLACK));
    if (insecure) {
        /* Operator opted out of authentication: type 1 (None), as before. */
        vga_print_color("  VNC: WARNING - --insecure: no authentication (security type None); operator-only, keep off untrusted nets\n",
                        VGA_COLOR(VGA_YELLOW, VGA_BLACK));
    } else {
        vga_print_color("  VNC: RFB VNC Authentication enabled (DES challenge/response, 3 s lockout after a failure)\n",
                        VGA_COLOR(VGA_LIGHT_GREEN, VGA_BLACK));
    }
    return true;
}

void vnc_start(void) { vnc_start_mode(false); }

void vnc_stop(void) {
    if (!vnc_running) return;
    tcp_conn_t* c = get_conn();
    if (c) tcp_close(c);
    vnc_running = false;
    vnc_conn_idx = -1;
    client_ready = false;
    vga_print_color("  VNC: Stopped\n", VGA_COLOR(VGA_YELLOW, VGA_BLACK));
}

bool vnc_is_running(void)       { return vnc_running; }
bool vnc_client_connected(void) { return client_ready; }
uint32_t vnc_frames_sent(void)  { return frames_sent; }

/* --------------------------------------------------------------------------
 * Poll — the heart of the server (non-IRQ context)
 * -------------------------------------------------------------------------- */
/* Re-arm the VNC listener in a free TCB slot and reset the RFB session. */
static bool vnc_relisten(void) {
    tcp_conn_t* nc = tcp_listen(VNC_PORT);
    if (!nc) { vnc_running = false; vnc_conn_idx = -1; return false; }
    int count;
    const tcp_conn_t* all = tcp_get_connections(&count);
    for (int i = 0; i < count; i++)
        if (&all[i] == nc) { vnc_conn_idx = i; break; }
    vnc_conn_gen = nc->gen;
    reset_session();
    return true;
}

void vnc_poll(void) {
    if (!vnc_running || vnc_conn_idx < 0) return;

    tcp_conn_t* c = get_conn();
    if (!c) {
        /* TCB slot freed (connection fully closed) — re-arm the listener. */
        vnc_relisten();
        return;
    }

    /* Remote closed (or our close progressed past ESTABLISHED): tear down and
     * re-listen so the next client isn't refused while we sit at the prompt. */
    if (c->closed || (c->connected && c->state != TCP_ESTABLISHED)) {
        tcp_close(c);
        vnc_relisten();
        return;
    }

    if (!c->connected) return;

    /* 1. ProtocolVersion: we speak first. We announce 3.8 and then follow
     *    whatever 3.x the client answers with (3.3 / 3.7 / 3.8). A failed
     *    login (Phase 55) stalls new handshakes until the lockout expires. */
    if (phase == RFB_WAIT_VERSION && !version_sent) {
        if ((int32_t)(lockout_until - system_ticks) > 0) return;   /* rate limit */
        send_raw(c, "RFB 003.008\n", 12);
        version_sent = true;
        return;
    }

    /* 2. Pull bytes off the wire. */
    drain_rx(c);

    /* 3. Advance the handshake / parse messages. */
    if (phase == RFB_WAIT_VERSION) {
        if (plen < 12) return;          /* client version "RFB 003.00x\n" */
        client_minor = 3;
        if (memcmp(pbuf, "RFB 003.00", 10) == 0) {
            if (pbuf[10] == '7') client_minor = 7;
            else if (pbuf[10] == '8') client_minor = 8;
        }
        pbuf_drop(12);
        uint8_t sectype = vnc_insecure ? 1 : 2;   /* 1 = None, 2 = VNC Authentication */
        if (client_minor >= 7) {
            /* 3.7+: server lists its security types, client picks one. */
            uint8_t list[2] = { 1, sectype };
            send_raw(c, list, 2);
            phase = RFB_WAIT_SECTYPE;
        } else {
            /* 3.3: server dictates the type as a u32; for type 2 the 16-byte
             * challenge follows immediately. */
            uint8_t sec[4];
            put32(sec, sectype);
            send_raw(c, sec, 4);
            if (sectype == 2) start_challenge(c);
            else phase = RFB_WAIT_CLIENTINIT;
        }
    }
    if (phase == RFB_WAIT_SECTYPE) {
        if (plen < 1) return;           /* client's chosen type */
        uint8_t chosen = pbuf[0];
        pbuf_drop(1);
        uint8_t want = vnc_insecure ? 1 : 2;
        if (chosen != want) {
            send_security_result(c, false, "unsupported security type");
            tcp_close(c);
            vnc_relisten();
            return;
        }
        if (want == 2) {
            start_challenge(c);
        } else {
            if (client_minor >= 8) send_security_result(c, true, NULL);  /* 3.8: None still gets a result */
            phase = RFB_WAIT_CLIENTINIT;
        }
    }
    if (phase == RFB_WAIT_AUTH) {
        if (plen < 16) return;          /* 16-byte DES response */
        bool ok = vnc_check_response(pbuf);
        pbuf_drop(16);
        if (ok) {
            send_security_result(c, true, NULL);
            phase = RFB_WAIT_CLIENTINIT;
            syslog_add("vnc: client authenticated");
        } else {
            auth_failures++;
            lockout_until = system_ticks + VNC_LOCKOUT_TICKS;
            send_security_result(c, false, "VNC authentication failed");
            syslog_add("vnc: authentication FAILED");
            tcp_close(c);               /* FIN right behind the result */
            vnc_relisten();             /* next client re-does the handshake */
            return;
        }
    }
    if (phase == RFB_WAIT_CLIENTINIT) {
        if (plen < 1) return;           /* ClientInit shared-flag */
        pbuf_drop(1);
        send_server_init(c);
        phase = RFB_READY;
        client_ready = true;
        mark_all_dirty();
        req_pending = true;             /* paint the first frame promptly */
    }
    if (phase != RFB_READY) return;

    parse_client(c);

    /* 4. Detect change + service a pending update. */
    hash_tiles();
    if (req_pending) send_update(c);

    /* 5. Push clipboard changes outward. */
    clipboard_out(c);
}
