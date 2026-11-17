---
name: headless-test-harness-gotchas
description: "Non-obvious gotchas when driving NexusOS headlessly via the QEMU monitor (screendump path spaces, GUI-launcher commands, key-drop, detached launch)"
metadata: 
  node_type: memory
  type: reference
  originSessionId: d4fbbe73-e63e-4c10-8fc9-323daa2b07b0
---

Driving NexusOS in headless QEMU (9.1.0) for phase verification. Four traps that
each silently wasted a run — all are harness issues, NOT OS bugs. See
[[nexusos-boot-and-test]].

- **Aurora (2026-10-08): Esc no longer exits the desktop — send `f12`** (NexusOS menu
  "Exit to Console"). GUI-launcher shell commands still enter the desktop; leave with F12.
  The console is a centred Terminal window: its 640x400 text area is at (640,354) at 1080p
  (crop there, not at 0,0). Login: type name+Enter, password+Enter (same as before).
  `mouse_move` is ~1:1 and clamps at screen edges. Boot reaches the login in ~7 s.

- **Claude's Bash-tool heredocs mangle backslash escapes** (`'\0'` became a real NUL byte
  in a .c file, `\n` matches failed). Write edit scripts with the Write tool and run them,
  or use the Edit tool, whenever the text contains backslashes.

- **`screendump <file>` splits the filename on spaces.** The project path is
  `C:\Users\admin\Desktop\New folder\NexusOS` — the space in "New folder"
  truncates every dump (writes to a bogus name → zero screenshots). Dump to a
  **space-free scratch dir** (e.g. `C:\nexustest\foo.ppm`) then convert with PIL.
  Note: with `-vga none -device virtio-vga`, screendump writes **PPM bytes even
  with a `.png` extension** — open with PIL and `.convert('RGB').save(png)`.

- **Many "info" commands launch the DESKTOP, not text output.** In shell.c these
  all call `cmd_gui()`→`desktop_run()`: `gui calc files sysmon settings notepad
  taskmgr calendar music paint help2 minesweeper sysinfo todo pong search tetris
  hexview contacts colors`. Running ANY of them from a text-shell batch drops you
  into the GUI desktop's Terminal app (prompt `nexus@os:~$`), which only knows a
  few commands — every later real command then returns "Unknown command". Keep
  GUI launchers OUT of text-shell batches. Real fullscreen handlers (NOT desktop):
  `snake doom breakout gamepad sprites gpubench`.

- **First keystroke after a fullscreen app exits gets dropped**, and a leftover
  `q`/`esc` from the previous quit leaks into the next line (e.g. `snake`→`nake`,
  `breakout`→`qbreakout`). Fix: before each command send ~12 `backspace`es to
  clear the line, then type slowly (~60 ms/key), then `ret`. Screenshot a game
  BEFORE sending its exit key. Snake waits for first input before painting its
  board — a too-early shot shows just the prompt on black.

- **Launch the driver fully DETACHED** (`Start-Process ... -RedirectStandardOutput
  -WindowStyle Hidden -PassThru`), not via a tool-shell background job — the
  latter gets killed when the turn/session boundary tears down the shell tree,
  killing the Python mid-run (QEMU survives, orphaned). Detached survives.

- **The space in the project path ALSO breaks the QEMU `-drive file=` launch
  arg, not just `screendump`.** Launching via PowerShell `Start-Process -ArgumentList`
  with `file=$img,format=raw,...` where `$img` = `...\New folder\nexus.img` makes
  QEMU parse only up to the space (`Could not open 'C:\Users\admin\Desktop\New'`)
  and **exit instantly** — which looks exactly like an early boot crash (process
  gone in <1s, monitor socket refused). Fix: wrap the path in escaped quotes
  inside the arg: `"file=`"$img`",format=raw,if=floppy"`. Same for `-serial
  file:"..."`. Verify QEMU stayed alive before concluding the OS crashed.

- **The active VESA mode set by boot2 is 24bpp, not 32bpp.** Serial fb_init dump
  shows bpp=`0x18`=24, pitch=`0xC00`=3072 (=1024×3) at fb `0xFE000000`. So the
  framebuffer present hits the **24bpp byte-store conversion path**, not the cheap
  32bpp memcpy — relevant for any present/blit perf work. The back buffer (`fb_back`
  at 0x500000) is always 32bpp; only the final copy to the hardware front buffer
  converts to 24bpp.

- **The runtime `!EX:00000008` (#DF) only reproduces under abusive thrash**
  (11 back-to-back `desktop_run` enter/Esc cycles + 7 rapid game launches with
  synthetic keys). In isolation every game + GPU path runs fault-free; the
  post-#DF eip resolved into unrelated `appstore` code = stale value, consistent
  with a transient stack issue under thrash (the heap/stack/fb_back layout
  landmine), not a per-feature crash. The text-shell campaign (60 cmds) and clean
  game runs had zero faults.

- **Phase 55 harness facts:** recovered kernel faults print `!KR:` (NOT a crash; only `!EX:` is fatal). The agents' extended harnesses live in the session scratchpad worktrees: ws_usb2/harness.py (`USBHC`, `USBDEV="kbd,hub(mouse,storage)"`, `mon:`, controller id `hc` so hot-plug is `device_add usb-kbd,bus=hc.0,port=N`), ws_net/harness.py (`HOSTFWD="hp:gp,..."`, `run:<nettests func>`), ws_persist/harness.py (`reboot`, `KEEPDISK=1`, `NODISK=1`). Python stdout is buffered when redirected: a long run shows no output until it exits. Start-Process with a nested-quoted path inside `-Command` splits on the space in "New folder" — pass `-ArgumentList` items instead.
