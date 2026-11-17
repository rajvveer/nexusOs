/* ============================================================================
 * NexusOS — VNC / RFB Remote Desktop Server (Header) — Phase 45
 * ============================================================================
 * Era 6 (Polish). The "remote desktop" pillar of Phase 45 "Cloud & Sync".
 *
 * A from-scratch RFB 3.3 (VNC) server. Streams the live framebuffer
 * (fb_get_backbuffer(), native-size XRGB) to a standard VNC client (TigerVNC,
 * vinagre/gtk-vnc, RealVNC, macOS Screen Sharing, vncdotool) and injects the
 * client's keyboard + pointer events back into the local input stack. It also
 * carries the clipboard both ways (RFB ClientCutText / ServerCutText), which
 * doubles as the Phase 45 "clipboard sync" pillar over the remote-desktop link.
 *
 * Design (validated by a design panel — see SESSION_LOG):
 *   - Phase 55: announces RFB 003.008 and speaks 3.3, 3.7 or 3.8 (whatever
 *     the client answers). Security is "VNC Authentication" (type 2): a
 *     16-byte random challenge, DES-encrypted by the client with the VNC
 *     password (bit-reversed bytes, zero-padded to 8 — the VNC quirk), and
 *     compared by the server (kernel/des.c). The VNC password is separate from
 *     the account passwords (those are only stored as salted hashes, so no
 *     challenge/response can be derived from them); `vncpasswd <pw>` stores it
 *     DES-obfuscated in /etc/vncpasswd (mode 0600, root), the same format as a
 *     real ~/.vnc/passwd. A failed login closes the socket and locks new
 *     handshakes out for 3 s. `vnc` refuses to start without a password unless
 *     `vnc --insecure` (type 1 None, with a warning) is used.
 *   - PIXEL_FORMAT = 32bpp / depth 24 / little-endian / true-colour with
 *     R<<16 G<<8 B<<0 — byte-identical to our 0x00RRGGBB back buffer, so
 *     pixels go on the wire with ZERO conversion.
 *   - Raw encoding only. Full-width (x=0, w=fb width) dirty row-bands keep the
 *     back buffer contiguous, so each band is a zero-copy tcp_send slice.
 *   - 32-bit FNV-1a tile hashing (sub-sampled, rolling per poll) detects
 *     change; updates are pull-model (one per FramebufferUpdateRequest) and
 *     banded (<=32 KB per tcp_send) to fit the uint16_t length and
 *     the 18 Hz polled loop. Integer math only (no 64-bit / libgcc).
 *   - vnc_poll() is called from a non-IRQ loop (the `vnc` serve command and
 *     desktop_run per-frame) — never from IRQ (tcp_send / no blocking there).
 *
 * Standard VNC port 5900 (forward host:5900 -> guest:5900 in build.bat).
 * ============================================================================ */

#ifndef VNC_H
#define VNC_H

#include "types.h"

#define VNC_PORT 5900   /* standard VNC display :0 */

/* Lifecycle — mirrors httpd/rshell. */
void vnc_init(void);     /* one-time init at boot                            */
void vnc_start(void);    /* begin listening on VNC_PORT (authenticated)      */
void vnc_stop(void);
void vnc_poll(void);     /* service the protocol; call from a main loop      */
bool vnc_is_running(void);

/* ---- Phase 55: RFB VNC Authentication ----
 * vnc_start_mode(false) == vnc_start(): refuses (returns false, prints why)
 * when no VNC password is set. vnc_start_mode(true) is `vnc --insecure`:
 * security type None, with the old operator-only warning. */
bool vnc_start_mode(bool insecure);
bool vnc_has_password(void);                 /* /etc/vncpasswd present?     */
int  vnc_set_password(const char* password); /* 0 ok, -1 bad arg, -2 fs err */
bool vnc_clear_password(void);
uint32_t vnc_auth_failures(void);
/* Shell: `vncpasswd <password>` / `vncpasswd clear` (root only). */
void vnc_cmd_passwd(int argc, char* argv[]);

/* True once a client has completed the RFB handshake (for the status line). */
bool vnc_client_connected(void);

/* Stats for the `vnc` status command. */
uint32_t vnc_frames_sent(void);

#endif /* VNC_H */
