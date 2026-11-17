---
name: polled-tcp-server-servicing
description: "NexusOS polled TCP servers (vnc/sync/rshell/httpd) — re-listen on client CLOSE and pump the poll at the text shell, else clients hang/RST"
metadata: 
  node_type: memory
  type: project
  originSessionId: 8354ac3a-d200-4aba-9b6f-a9b615d04b0a
---

NexusOS network servers (`httpd`, `rshell`, and the Phase-45 `vnc`/`sync`) are
**polled, single-connection listeners**: `tcp_listen(port)` grabs one TCB slot,
a `*_poll()` (non-IRQ) drains `conn->rx_buf`/`rx_len` and writes via `tcp_send`
(which transmits synchronously — QEMU delivers during the poll). Per-conn RX/TX
buffers are only 1024 B; `tcp_send` len is `uint16_t`.

Two non-obvious requirements, both learned the hard way in Phase 45:

1. **Re-listen on client CLOSE, not just on TCB-inactive.** A disconnect leaves
   the TCB in FIN_WAIT (still `active`) for a while, so re-arming only when
   `!c->active` means the *next* client is RST'd by QEMU's NAT (no listener).
   Detect `c->closed || (c->connected && c->state != TCP_ESTABLISHED)`,
   `tcp_close` our side, and **immediately re-listen** (`vnc_relisten`/
   `sync_relisten`). `rshell`/`httpd` only survive without this because something
   pumps them every tick.

2. **A backgrounded server needs its poll pumped at the TEXT shell.** Polls run
   per-frame from `desktop_run`, but the raw shell blocks in `keyboard_getchar()`
   (spins on `hlt`), starving any server started from the shell. Fix:
   `keyboard_set_idle_hook()` — `keyboard_getchar` calls it each wait iteration;
   `shell_run` installs `shell_idle_pump()` = `net_poll`+`vnc_poll`+`sync_poll`.
   Any new shell-driven background service must use this hook, or be an
   interactive blocking command (like `vnc serve`).

Verify servers from the host over QEMU hostfwd (add `hostfwd=tcp::PORT-:PORT` to
`-netdev`): `vnc_capture.py` (RFB 3.3 client → PNG, reads real `fb_back` — more
reliable than the monitor `screendump`) and `sync_client.py`. See
[[nexusos-boot-and-test]] for the build/drive workflow and
[[nexusos-single-address-space]] for why cooperative (non-IRQ) polling is safe.
