---
name: aurora-ui-architecture
description: "NexusOS 5.1 \"Aurora\" macOS-style desktop (2026-10-08) — file map, generated fonts, -O2 hot files, action ids, key codes, F12 exit, how to add an app"
metadata:
  node_type: memory
  type: project
  originSessionId: 37d1b0eb-50a3-46b5-aea1-6f0971a47e47
  modified: 2026-10-08T08:49:21.216Z
---

**UPDATE 2026-10-08 (apps UI): `appui.c/h` is now a pixel macOS kit** (same cell-coord API, renders AA text / dark panels / pill fields / blue selection — upgrades ALL ~28 apps at once; plus `appui_px_*` pixel widgets + `appui_content(id,...)`). Bespoke apps: Calculator (clickable button grid), Settings (sidebar+panes), Activity Monitor (gauges+process table), Notes (caret). Windows gained `window_set_on_click`/`window_send_click` (content clicks -> app, pixel coords) and `window_content_px`. appui is in the -O2 build list. See [[phase54-isolation-usb-folders]].

On 2026-10-08 the user asked to "fix everything and make it exactly like a Mac". The
desktop was rebuilt as **Aurora** (version macro `kernel/version.h`: NexusOS 5.1 Aurora).

- **Files:** `macui.c/h` (toolkit: AA text, rrect/circle/line/tri, soft `ui_shadow`, procedural
  wallpaper cached at `WALLPAPER_CACHE_ADDR` 0x01800000, Big Sur icons `ui_icon`, pointer,
  boot splash, app registry `APPS[]`), `menubar.c/h`, `dock.c/h` (rewritten), `spotlight.c/h`
  (Spotlight + Launchpad), `login.c` (GUI login + `login_lock_screen`), restyled
  `window.c`, `notify.c`, `context_menu.c` (now **pixel** coords), `switcher.c`, `shortcuts.c`,
  `notifcenter.c`. Old `taskbar.c`/`icons.c`/`start_menu.c` are still linked but no longer drawn.
- **Fonts/pointer are generated:** `python gen_uifont.py` writes `kernel/uifont.h` (Aileron
  via Pillow's bundled font = UI; Cascadia Mono from C:\Windows\Fonts = 8x16 cell glyphs,
  used by `font_draw_char_fixed` for ASCII 32..126). Only ASCII exists in the UI fonts —
  em dashes/×/arrows silently render as gaps.
- **build.bat** recompiles `macui menubar dock spotlight framebuffer gfx font gui window` at
  **-O2** after the -O0 loop (per-pixel code is far too slow at -O0). New .c files must go
  in the link list AND (if hot) that -O2 list.
- **Adding an app to dock/Launchpad/Spotlight:** add a row to `APPS[]` in macui.c (name,
  desktop action id, ICON_*, window title for the running dot); first 10 rows = dock.
  Special actions are `UI_ACT_*` (macui.h), handled in desktop.c `handle_action`.
- **Keys (keyboard.h KEYC_*):** F1..F12 = 0x90..0x9B (no longer raw scancodes, which
  collided with ';'), Ctrl/Win+Space = 0x9C Spotlight, Ctrl+arrows 0x84-0x87, Ctrl+F5-F8
  0x88-0x8B, Delete 0x7F. **Esc no longer leaves the desktop — F12 / NexusOS menu
  "Exit to Console" does.** The console is drawn as a centred 80x25 Terminal window.
- Palette (framebuffer.c `vga_palette`) is macOS dark; index 0 = window bg 0x1E1E20.
- GUI cell grid is runtime-sized (`gui_cols/gui_rows`, 240x67 at 1080p, max 256x68).

Related: [[desktop-dirty-rect-render]], [[nexusos-1080p-memory-layout]], [[headless-test-harness-gotchas]].
