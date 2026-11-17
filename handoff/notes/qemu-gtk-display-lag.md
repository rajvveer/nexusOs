---
name: qemu-gtk-display-lag
description: "User's '2 fps mouse lag' (2026-10-09) was QEMU's GTK display backend on Windows (host redraws 2-4 fps while the guest presented 49/s); fixed with -display sdl in build.bat run/debug + a dock/menu-bar hover band fast path (Phase 56). How to measure guest vs host fps."
metadata:
  node_type: memory
  type: project
  originSessionId: 37d1b0eb-50a3-46b5-aea1-6f0971a47e47
  modified: 2026-10-09T06:13:35.640Z
---

User reported "mouse movement 2 fps, bahut laggy" right after Phase 55. Diagnosis (all headless
A/B runs, scratchpad/fpsrun.py + hostfps.py):

- **Guest side was never slow:** a serial FPS probe (desktop loop iterations / cursor presents /
  full composes per PIT second) showed 49 cursor presents/s for 50 monitor mouse_move/s in
  BOTH the Phase 55 build and the pre-55 base2 build, with virtio-vga and -vga std, with
  `-audiodev dsound` too. Phase 55's per-frame additions (kguard_try, usb_poll once per tick,
  persist_poll, httpd_poll) are negligible.
- **Host side was the lag:** with `-display gtk` (what `build.bat run` used) the QEMU window
  on this Windows host repaints only **2-4 frames/s** (measured by grabbing the window with
  PIL ImageGrab while streaming mouse moves), even with `zoom-to-fit=off`; `gtk,gl=on` crashes
  QEMU on Windows; **`-display sdl` gives 17+ host frames/s** (capture-limited). Fix:
  build.bat `run`/`debug` now pass `-display sdl`.
- **Second real cost found:** a full `compose()` of the 1080p desktop takes ~150-200 ms under
  TCG, and dock magnification forced one per pointer step (4-5 composes/s, loop choked).
  Phase 56 fast path in desktop.c: compose() saves the base layer (wallpaper+widgets+windows)
  under the dock band (dock_top()-150..bottom, static 1920x256 buffer in .bss) and a hover-only
  change (`dock_dirty`/`bar_dirty`) restores the band, redraws just the dock / menu bar and
  presents those rows (`partial_ok` guards: no overlays/menus/toasts/Launchpad, else full).
  Result: 0 full composes + 10-12 band presents/s + 37-44 cursor presents/s over the dock.
- Measuring tips: QEMU windows launched from the hidden tool shell start **minimized**
  (GetWindowRect = -32000); pass a STARTUPINFO with SW_SHOWNORMAL and ShowWindow(SW_RESTORE)
  before ImageGrab. GDI capture of the SDL (D3D) window may miss updates, so SDL numbers are a
  floor. Dock hover sweeps must cross icon boundaries (dock_hover_changed fires only when the
  pointer x or inside-state changes); the dock icons span x~600..1320 at y~1035.

See [[phase55-kernel-usb-net-persist]], [[desktop-dirty-rect-render]], [[headless-test-harness-gotchas]].
