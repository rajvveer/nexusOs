---
name: phase57-in-progress
description: "Phase 57 (started 2026-10-09 ~12:00) — user wants \"all apps / Chrome / 64-bit\"; chosen path = real internet + own userland; 4 agents in worktrees (ws_tls, ws_browser, ws_mem, ws_apps) off snapshot base3; merge plan + decisions; delete when merged"
metadata:
  node_type: memory
  type: project
  originSessionId: 37d1b0eb-50a3-46b5-aea1-6f0971a47e47
  modified: 2026-10-09T06:36:34.173Z
---

**Status 2026-10-09 21:45:** second limit hit ~17:00 (reset 21:30); resumed again 21:42. Worktree builds: tls 960,876 B (smoke run done, full campaign pending), browser 919,948 B (5 test runs, debugging image loads), mem 878,956 B (bootloop running), apps unbuilt (21 files). NOTE: merged size will exceed the old 1,015,808 cap — the merge DEPENDS on ws_mem raising it to 1,441,792 (boot2 KERNEL_SECTORS 2816); merge ws_mem first, bootloop, then the rest. Earlier 16:36: all 4 agents hit the API session limit ~12:30 (reset 16:10) with partial work (tls: crypto/bignum/x509/roots + tcp edits, no netstream/tls.c; browser: webengine+browser.c+compat, not integrated; mem: 24 files edited, unbuilt; apps: fatput.py + usys_abi.h only); all resumed via SendMessage at 16:35 with per-worktree state summaries. Expect more limit hits — resume the same way.

**User ask:** "Chrome aur saare Win/Mac/Linux apps chalne chahiye, 64-bit bana" → after I explained
that bitness is not the blocker (missing OS ABIs are) they said "jo bhi best hai wo karo". Decision:
build the realistic path — real internet + NexusOS's own POSIX-ish userland so real C apps compile
and run — and DEFER the x86-64 port (it would freeze everything and unlock no apps). Say this
clearly in the final report; offer the 64-bit port as a later phase.

**Agents (all from snapshot `scratchpad/base3`, merge with mergeplan.py base3):**
1. ws_tls — tcp.c retransmission/flow control/16 KB window, `netstream.c` implementing the shared
   `kernel/netstream.h` (ns_open/ns_poll/ns_read/ns_fetch), TLS 1.3 client (X25519, AES-128-GCM,
   SHA-256/HKDF, RSA-PSS/PKCS1 + ECDSA P-256 verify, X.509, built-in root store), `wget`/`curl`/
   `tlsinfo`. Ports 56100-56119, host 18100-18199.
2. ws_browser — graphical "Nexus Browser" (webengine layout, images, links, scroll, address bar,
   about:home), consumes netstream.h via a temporary `kernel/netstream_compat.c` that MUST BE
   DELETED at merge (replaced by the real netstream.c). `browser_poll()` in desktop loop.
   Ports 56120-56139, host 18200-18299.
3. ws_mem — heap 1 MB→4 MB at 0x400000, ramfs file cap 4 KB→256 KB, persist slots 1 MiB→1.5 MiB
   (+ "too large" skip), kernel size cap 1,015,808→1,441,792 (boot2 KERNEL_SECTORS 2816),
   `/home/<u>/Downloads`, `sum`, `mkfile`. Ports 56140-56159.
4. ws_apps — userland: extended syscall ABI (open/lseek/stat/spawn/waitpid/ioctl termios/...),
   FPU save/restore, VT100 subset in console + GUI terminal, foreground jobs in the GUI terminal,
   `userland/` mini-libc + crt0 + user.ld + fatput.py (FAT32 injector that must not touch LBA ≥
   20480), ports of Lua 5.4.7, kilo, 2048, tetris, coreutils-lite `sh`, App Store/Launchpad
   entries, bare-name → /disk/bin lookup. Ports 56160-56179.

`kernel/netstream.h` was added to the real repo + base3 + all worktrees (shared contract).
AGENT_COMMON.md updated for Phase 57 (heap 1 MB warning, !KR = bug, -display sdl, slirp host
10.0.2.2). After merge: delete netstream_compat.c, rebuild, bootloop, campaign, re-run each
agent's tests, README + screenshots, final Hinglish report, keep NOT committing unless asked.
Context: Phase 55 merged+verified; Phase 56 = dock/menu-bar band fast path + `-display sdl`
(see [[qemu-gtk-display-lag]]).
