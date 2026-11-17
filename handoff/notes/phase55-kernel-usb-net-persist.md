---
name: phase55-kernel-usb-net-persist
description: "Phase 55 (2026-10-08/09) — kernel W^X + kguard crash containment (#DF task gate), full USB stack (xHCI/EHCI/OHCI/UHCI + hubs + hot-plug), VNC DES auth + real httpd (+TCP TIME_WAIT fix), persistent ramfs (A/B slots at LBA 26624, users too). 4 worktree agents, merged + verified; not committed."
metadata:
  node_type: memory
  type: project
  originSessionId: 37d1b0eb-50a3-46b5-aea1-6f0971a47e47
  modified: 2026-10-08T21:56:11.967Z
---

User said "sab kr" to the four Phase 54 leftovers. Done by 4 subagents in worktrees
(ws_kern, ws_usb2, ws_net, ws_persist off snapshot base2; see
[[parallel-agent-worktrees]]), merged with mergeplan.py (only 2 trivial keep-both
conflicts), fully-merged build **874,860 B / 1,015,808, 0 warnings**; verified on the
merged build: 66/66 shell campaign, net 57/57 host checks, USB matrix (UHCI/xHCI/
xHCI+hub/EHCI-companion/OHCI), crashtest x4 recovered, persist across reboot, GUI
app-crash contained, bootloops clean. Agents hit the API session limit twice
(resets 21:50 / 03:00 IST) — resuming them with SendMessage kept their context.

- **Kernel (kguard.c/h, kguard_asm.asm, crashtest.c):** `.text/.rodata` read-only
  (linker.ld page-aligns .data; `paging_protect_kernel()`), page 0 unmapped (nothing
  reads <0x1000; VESA info is at 0x9000), 16 KB guard band 0x300000-0x304000 under the
  boot stack. kguard = setjmp/longjmp recovery contexts (depth 8) armed around shell
  commands, each desktop frame, every window draw/key/click, GUI-terminal commands.
  Fault handlers call `kguard_should_recover/kguard_recover` AFTER `uproc_fault`;
  recovered faults log `!KR:<int> <err> <eip> <addr> <what>` (harness treats only `!EX:`
  as fatal). #DF goes through a **task gate** (own TSS, GDT sel 0x30) so kernel-stack
  overflow is recoverable — **a hardware task switch loads CR3 from the TSS but never
  saves it; the main TSS must carry the current CR3** (first version triple-faulted).
  Still fatal: faults in IRQ handlers, during boot/login, or while a ring-3 process is
  current. `crashtest <null|gpf|ud|div|wp|stack|app|stats>`.
- **USB (usb.c core + usb_uhci/ohci/ehci/xhci/hub.c):** `usb_hcd_t` ops table, devices
  carry hcd/port/route/parent; `paging_map_mmio()` for MMIO BARs (before uproc_init);
  PIT-readback `usb_delay_ms` (old spin loop was ~20 us on real CPUs). EHCI hands
  FS/LS ports to the companion via PortOwner; xHCI route strings/TT fields; hub status
  change EP polled from `usb_poll()` (via usb_hid_poll, so no new call sites).
  HID: F1..F12 -> KEYC_F(n) (F12 from a USB keyboard now exits the desktop), arrows,
  Spotlight, typematic. QEMU: `sendkey` goes to the USB keyboard when one exists.
  Harness (ws_usb2/harness.py): `USBHC=uhci|xhci|ehci|ehci-companion|ohci`,
  `USBDEV="kbd,hub(mouse,storage)"`, steps `mon:<cmd>` (controller id is `hc`, so hot-
  plug is `device_add usb-kbd,bus=hc.0,port=4`), `grep:`. Low-speed devices cannot be
  emulated by QEMU (LS paths unverified); no isochronous; polled only.
- **Net:** des.c (FIPS-46, `des selftest`), VNC RFB 3.3/3.8 type-2 auth with
  bit-reversed-key DES, `vncpasswd` -> `/etc/vncpasswd` (0600), `vnc --insecure`, 3 s
  lockout; httpd rewritten: HTTP/1.0 static server over ramfs (`/www` docroot,
  listings, `/status`, `/api/status`, `/fs/<abs>` + PUT with HTTP Basic auth via
  users_authenticate + vfs_check_perm, 1 s lockout), `httpd stop|status|root`.
  **httpd_poll was never called before Phase 55** (httpd did not work at the console).
  tcp.c: server-side close leaked TIME_WAIT TCBs forever (8 slots) -> freed when the
  peer's FIN is ACKed + 30 s reaper; orphan TCBs adopted by the next tcp_listen on that
  port (listener gap between IRQ-time SYN and re-listen); `gen` counter so servers
  never mistake a recycled slot. rshell now re-listens on close like vnc/sync.
- **Persistence (persist.c/h):** window LBA 26624..30719 on `hda` (NPFS ends 24802;
  FAT32 <20480), slots A/B x 2048 sectors, header {NXRS, gen, counts, CRC32 payload,
  CRC32 header} written LAST; save = lower-gen slot, restore = higher valid gen. Nodes
  created before `persist_boot_done()` are BUILTIN (not saved unless modified);
  deleting a built-in records a tombstone (max 16). Users table serialized to
  `/etc/passwd.db`; `users_import()`. Autosave 2 s after a mutation from the shell idle
  pump + desktop loop; `persist_flush()` inside power_restart/shutdown + cmd_reboot.
  `persist [status|save|on|off|restore|dev <blk>]`. ~3 ms per save (2 ATA flushes).
  No disk (ISO/VMware CD boot) -> disabled, no crash. chmod/chown now resolve paths.
- README "What's new"/limitations/features updated + 6 screenshots (screenshots/phase55-*).
- Final test artifacts: C:\nexustest\final (usb matrix, net, kp, camp, gui, loops).

See [[phase54-isolation-usb-folders]], [[nexusos-single-address-space]],
[[headless-test-harness-gotchas]], [[polled-tcp-server-servicing]].
