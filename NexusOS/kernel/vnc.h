/* ============================================================================
 * NexusOS — VNC / RFB Remote Desktop Server (Header) — Phase 45
 * ============================================================================
 * Era 6 (Polish). The "remote desktop" pillar of Phase 45 "Cloud & Sync".
 *
 * A from-scratch RFB 3.3 (VNC) server. Streams the live framebuffer
 * (fb_get_backbuffer(), 1024x768 XRGB) to a standard VNC client (TigerVNC,
 * vinagre/gtk-vnc, RealVNC, macOS Screen Sharing, vncdotool) and injects the
 * client's keyboard + pointer events back into the local input stack. It also
 * carries the clipboard both ways (RFB ClientCutText / ServerCutText), which
 * doubles as the Phase 45 "clipboard sync" pillar over the remote-desktop link.
 *
 * Design (validated by a design panel — see SESSION_LOG):
 *   - RFB 003.003, security type 1 (None): the simplest path every client
 *     accepts. Server dictates security; no SecurityResult, no type byte.
 *   - PIXEL_FORMAT = 32bpp / depth 24 / little-endian / true-colour with
 *     R<<16 G<<8 B<<0 — byte-identical to our 0x00RRGGBB back buffer, so
 *     pixels go on the wire with ZERO conversion.
 *   - Raw encoding only. Full-width (x=0, w=1024) dirty row-bands keep the
 *     back buffer contiguous, so each band is a zero-copy tcp_send slice.
 *   - 32-bit FNV-1a tile hashing (sub-sampled, rolling per poll) detects
 *     change; updates are pull-model (one per FramebufferUpdateRequest) and
 *     banded (<=8 rows / 32 KB per tcp_send) to fit the uint16_t length and
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
void vnc_start(void);    /* begin listening on VNC_PORT                      */
void vnc_stop(void);
void vnc_poll(void);     /* service the protocol; call from a main loop      */
bool vnc_is_running(void);

/* True once a client has completed the RFB handshake (for the status line). */
bool vnc_client_connected(void);

/* Stats for the `vnc` status command. */
uint32_t vnc_frames_sent(void);

#endif /* VNC_H */
