/* ============================================================================
 * NexusOS — VNC / RFB Remote Desktop Server — Phase 45
 * ============================================================================
 * See vnc.h for the design. Mirrors rshell.c's listener structure: one
 * tcp_listen() TCB slot, a poll() that drains conn->rx_buf each call and
 * re-listens after a client drops.
 *
 * INTEGER MATH ONLY — no 64-bit multiply/divide (no libgcc in this
 * freestanding kernel). The largest product is the pixel index
 * y*1024+x (< 786432, fits uint32_t) and a band size rows*1024*4
 * (<= 32768, fits uint16_t). Never compute a whole-frame byte total.
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

extern volatile uint32_t system_ticks;

/* --------------------------------------------------------------------------
 * Geometry / tiling
 * -------------------------------------------------------------------------- */
#define VNC_FB_W    1024
#define VNC_FB_H    768
#define TILE        64
#define TILES_X     (VNC_FB_W / TILE)       /* 16 */
#define TILES_Y     (VNC_FB_H / TILE)       /* 12 */
#define TILE_COUNT  (TILES_X * TILES_Y)     /* 192 */
#define HASH_PER_POLL 24                     /* tiles hashed per poll          */
#define BAND_ROWS   8                        /* scanlines per tcp_send band    */
#define MAX_SEND_ROWS 64                     /* per-poll send cap (~256 KB)    */

/* --------------------------------------------------------------------------
 * RFB handshake phase
 * -------------------------------------------------------------------------- */
typedef enum { RFB_WAIT_VERSION, RFB_WAIT_CLIENTINIT, RFB_READY } rfb_phase_t;

/* --------------------------------------------------------------------------
 * Server state
 * -------------------------------------------------------------------------- */
static bool        vnc_running = false;
static int         vnc_conn_idx = -1;
static rfb_phase_t phase = RFB_WAIT_VERSION;
static bool        version_sent = false;

static uint8_t     pbuf[300];     /* protocol parse accumulator              */
static int         plen = 0;
static uint32_t    skip_bytes = 0;/* remainder of an oversized message       */

static bool        req_pending = false;
static bool        req_incremental = false;

static uint32_t    tile_hash[TILE_COUNT];
static uint16_t    hash_cursor = 0;
static int         dirty_y0 = 0, dirty_y1 = 0;   /* dirty row span [y0, y1)   */

static const char* last_clip = NULL;
static uint32_t    frames_sent = 0;
static bool        client_ready = false;

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
    for (int i = 0; i < TILE_COUNT; i++) tile_hash[i] = 0xFFFFFFFFu;
    dirty_y0 = 0; dirty_y1 = VNC_FB_H;
}

/* --------------------------------------------------------------------------
 * ServerInit message (sent once after ClientInit)
 * -------------------------------------------------------------------------- */
static void send_server_init(tcp_conn_t* c) {
    uint8_t m[24 + 7];
    int p = 0;
    put16(m + p, VNC_FB_W); p += 2;        /* framebuffer-width  */
    put16(m + p, VNC_FB_H); p += 2;        /* framebuffer-height */
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
    if (!back) return;

    for (int n = 0; n < HASH_PER_POLL; n++) {
        int i  = (hash_cursor + n) % TILE_COUNT;
        int tx = i % TILES_X;
        int ty = i / TILES_X;
        uint32_t base_y = (uint32_t)ty * TILE;
        uint32_t base_x = (uint32_t)tx * TILE;

        uint32_t h = 2166136261u;
        for (int yy = 0; yy < TILE; yy += 4) {
            uint32_t row = (base_y + yy) * VNC_FB_W + base_x;
            for (int xx = 0; xx < TILE; xx += 4) {
                h ^= back[row + xx];
                h *= 16777619u;          /* 32-bit imul — no libgcc */
            }
        }
        if (h != tile_hash[i]) {
            tile_hash[i] = h;
            int y0 = ty * TILE, y1 = y0 + TILE;
            if (dirty_y1 <= dirty_y0) { dirty_y0 = y0; dirty_y1 = y1; }
            else { if (y0 < dirty_y0) dirty_y0 = y0; if (y1 > dirty_y1) dirty_y1 = y1; }
        }
    }
    hash_cursor = (uint16_t)((hash_cursor + HASH_PER_POLL) % TILE_COUNT);
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
    if (y + h > VNC_FB_H) h = VNC_FB_H - y;
    if (h <= 0) { req_pending = false; dirty_y0 = dirty_y1 = 0; return; }

    /* FramebufferUpdate header: type(1)=0, pad(1), num-rects(2)=1 */
    uint8_t hdr[4];
    hdr[0] = 0; hdr[1] = 0; put16(hdr + 2, 1);
    if (send_raw(c, hdr, 4) < 0) return;

    /* Rectangle header: x(2), y(2), w(2), h(2), encoding(4)=0 (Raw) */
    uint8_t rh[12];
    put16(rh + 0, 0);             /* x */
    put16(rh + 2, (uint16_t)y);   /* y */
    put16(rh + 4, VNC_FB_W);         /* w */
    put16(rh + 6, (uint16_t)h);   /* h */
    put32(rh + 8, 0);             /* encoding = Raw */
    if (send_raw(c, rh, 12) < 0) return;

    /* Pixel data, banded. Each band is a contiguous slice of the back buffer
     * (full-width => no copy). band_bytes <= 8*1024*4 = 32768 (uint16_t ok). */
    int row = 0;
    int sent_rows = 0;
    while (row < h && sent_rows < MAX_SEND_ROWS) {
        int rows = h - row;
        if (rows > BAND_ROWS) rows = BAND_ROWS;
        uint16_t band_bytes = (uint16_t)(rows * VNC_FB_W * 4);
        uint32_t off = (uint32_t)(y + row) * VNC_FB_W;   /* < 786432 */
        if (send_raw(c, &back[off], band_bytes) < 0) return;
        row += rows;
        sent_rows += rows;
    }

    frames_sent++;

    if (row >= h) {
        /* whole band sent — clear dirty state */
        req_pending = false;
        dirty_y0 = dirty_y1 = 0;
    } else {
        /* hit the per-poll cap — keep the rest dirty for the next request */
        dirty_y0 = y + row;
        /* dirty_y1 unchanged; client will re-request (or our next request) */
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
                uint32_t full = 8u + tlen;
                if (full > sizeof(pbuf)) {
                    /* take what text we have, skip the remainder */
                    uint32_t have = (uint32_t)plen - 8u;
                    clipboard_in(pbuf + 8, have);
                    skip_bytes = full - (uint32_t)plen;
                    plen = 0;
                    return;
                }
                if ((uint32_t)plen < full) return;
                clipboard_in(pbuf + 8, tlen);
                pbuf_drop((int)full);
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
}

/* --------------------------------------------------------------------------
 * Reset per-connection protocol state
 * -------------------------------------------------------------------------- */
static void reset_session(void) {
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
    for (int i = 0; i < TILE_COUNT; i++) tile_hash[i] = 0;
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

void vnc_start(void) {
    if (vnc_running) return;
    tcp_conn_t* conn = tcp_listen(VNC_PORT);
    if (!conn) {
        vga_print_color("  VNC: No free TCP slots\n", VGA_COLOR(VGA_LIGHT_RED, VGA_BLACK));
        return;
    }
    vnc_running = true;
    reset_session();

    int count;
    const tcp_conn_t* all = tcp_get_connections(&count);
    vnc_conn_idx = -1;
    for (int i = 0; i < count; i++)
        if (&all[i] == conn) { vnc_conn_idx = i; break; }

    vga_print_color("  VNC: Listening on port 5900\n", VGA_COLOR(VGA_LIGHT_GREEN, VGA_BLACK));
}

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

    /* 1. ProtocolVersion: we speak first. */
    if (phase == RFB_WAIT_VERSION && !version_sent) {
        send_raw(c, "RFB 003.003\n", 12);
        version_sent = true;
        return;
    }

    /* 2. Pull bytes off the wire. */
    drain_rx(c);

    /* 3. Advance the handshake / parse messages. */
    if (phase == RFB_WAIT_VERSION) {
        if (plen < 12) return;          /* client version */
        pbuf_drop(12);
        uint8_t sec[4];
        put32(sec, 1);                  /* security type 1 = None */
        send_raw(c, sec, 4);
        phase = RFB_WAIT_CLIENTINIT;
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
