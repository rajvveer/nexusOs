# NexusOS — Session Context & Handoff

> A complete context dump of the assisted development sessions. Era 5
> (Multimedia, Phases 38–42) ✅; Era 6 (Polish, Phases 43–48) ✅; **Era 7 (World
> Domination, Phases 49–51) is now COMPLETE — the ENTIRE 51-phase roadmap is
> done (51/51, 100%).** 🎉 Read this top-to-bottom to pick up where the work left
> off. Companion to `SESSION_LOG.md` (changelog) and `README.md` (the public
> project doc / roadmap).
> **Latest: Phase 51 (NPFS — NexusOS Persistent File System) added 2026-06-16,
> the FINAL phase — `npfs.c/h`: a from-scratch crash-consistent JOURNALING
> filesystem (write-ahead log + CRC32 commit records + mount-time replay), 128
> B inodes with direct+single-indirect blocks, in a reserved high-LBA window of
> the IDE disk (never auto-formats → FAT32 safe). Verified live: format/write/
> cat, `npfs crashtest` PASS (recovers a file after a simulated crash), and
> PERSISTENCE ACROSS REBOOT (files + mount counter survive a power cycle). 3
> bugs found & fixed in verification (bitmap over-read, multi-alloc-in-txn
> collision, 64 KB stack txn). Shell v36.0 / 142 commands. Phase 50 (v5.0 Grand
> Finale: `finale.c/h`, `urun`/`install`/`finale`, bootable `nexus.iso`) and
> Phase 49 (App Store, `appstore.c/h`, `store`) were the prior sessions. See
> SESSION_LOG.md for all three.**

---

## 1. What NexusOS is

A bare-metal **x86 (i386), 32-bit protected-mode** operating system written from
scratch in C and NASM assembly. Custom 2-stage bootloader → VESA 1024×768×32bpp
framebuffer → paging/heap/VFS → processes/scheduler → full TCP/IP stack → POSIX /
X11 / Win32 / macOS compatibility layers → package manager → scripting engine →
sound, images, video, GPU, gaming → (Era 6) **accessibility**. ~625 KB kernel
(loaded high at 1 MB, ceiling ~720 KB), runs in 16 MB RAM.

- 51-phase roadmap (`roadmap.txt`, mirrored in `README.md`). As of end of the
  latest session: **51 / 51 phases complete (100%)** — the roadmap is DONE. 🎉
- Eras: 1 Graphics (14–18), 2 OS Foundations (19–25), 3 Networking (26–30),
  4 Compatibility (31–37), 5 Multimedia (38–42) ✅, 6 Polish (43–48) ✅,
  **7 World Domination (49–51) ✅ — all phases complete.**

---

## 2. What this session did

> **Phase 48 — Performance Optimization ✅ (latest, 2026-06-15; Era 6 finale).**
> New `kernel/perf.c/.h` + an optimized `string.c` + a small boot-safety change
> in kernel.c. (1) **rep-string memcpy/memset**: `rep movsl`/`stosl` aligned
> fast paths (overlap-safe — only tcp.c overlaps, dst<src forward-safe) — the
> 3 MB `fb_flip` copy is the prize; measured ~2917/3198 MB/s. (2) **Boot-time
> measurement**: `perf_mark_boot_done()` stamps ticks at end-of-init; measured
> **3 ticks ≈ 165 ms, under 1 second** (PIT left at 18.2 Hz; ±55 ms resolution
> — reprogramming would break the codebase `ticks/18` assumption). (3) **Boot
> IRQ guard**: `volatile bool boot_init_done` — `system_ticks++` stays
> unconditional, but status-bar/net_poll/schedule defer until init finishes;
> mitigates the long-standing intermittent boot #DF (IRQ0 fires during init
> because `idt_load` does `sti` before init completes). (4) **Honest SMP/
> preempt status** (`smp`/`preempt`): single-CPU cooperative by design; the
> scheduler exists but is intentionally never started (no locks → preemption
> would corrupt heap/VGA/VFS). 3 commands (`perf`/`smp`/`preempt`), shell
> **v33.0 / 137 commands**. Adversarial review: 0 bugs. Full writeup
> `SESSION_LOG.md`; gotchas §3; files §6.

> **Phase 47 — Mobile/Embedded Mode ✅ (2026-06-15).** New
> `kernel/mobile.c/.h`, scoped honestly for QEMU i386. (1) **Touch gestures**:
> `mobile_poll()` reads the PS/2 mouse (read-only) and classifies down->move->up
> into tap/long-press/drag/swipe-{up,down,left,right}; driven from desktop,
> shell-idle, AND `vnc.c inject_pointer` (so remote VNC pointer events sample at
> full rate). (2) **Responsive orientation**: a LOGICAL portrait/landscape model
> — `mobile_screen_w/h()` swap to 768×1024 in portrait for layout-aware code
> (the framebuffer is NOT physically rotated). (3) **Low-power**: stretches the
> desktop idle-redraw cadence 18->72 ticks (~1s->~4s) via
> `mobile_redraw_interval()` so the CPU stays in HLT longer (real input still
> redraws immediately). (4) **ARM**: an honest status report (target is x86; no
> ARM toolchain; lists what a port needs). 5 commands (`gesture`/`orientation`/
> `lowpower`/`arm`/`mobileinfo` — `touch` was taken by file-create), shell
> **v32.0 / 134 commands**. Integer-only. Adversarial review found 0 bugs
> (re-entrancy of the 3-site mobile_poll refuted — same cooperative thread).
> Verified live: swipe-right/tap/swipe-up recognized, portrait=768×1024,
> lowpower=72 ticks, arm honest. Full writeup `SESSION_LOG.md`; gotchas §3;
> files §6.

> **Phase 46 — AI Assistant ✅ (2026-06-15).** New `kernel/assistant.c/.h`
> — an OFFLINE, rule-based assistant (no LLM). `ask "<plain English>"` tokenizes
> + scores the request against a ~55-entry intent table (keyword sets ->
> command templates), extracts an arg, and runs the matching command via
> `shell_exec_line` when confident (RUN_MIN=12, margin>=4); a **destructive
> guard** refuses to auto-run `rm`/`kill`/`chmod`/`firewall` without strong
> signal + a real arg. `ai [cmd]` explains commands from an embedded knowledge
> base; `find <query>` does smart file search over names AND contents (grep +
> snippets). Tab in `editor.c` autocompletes via `assistant_complete()` against
> keywords + buffer words. 3 shell commands (`ask`/`ai`/`find`), shell **v31.0 /
> 129 commands**. Integer-only (no 64-bit math in assistant.o). All four pillars
> verified live (ask->date/meminfo ran; ask->rm refused; find by content;
> Tab->return/while). One review bug fixed (editor autocomplete ac_inserted
> desync on a full line). Full writeup in `SESSION_LOG.md`; gotchas §3; files §6.

> **Phase 45 — Cloud & Sync ✅ (2026-06-14).** New `kernel/vnc.c/.h`
> (from-scratch **RFB 3.3 VNC remote-desktop server**, port 5900: zero-copy
> full-width Raw row-bands of `fb_back`, 32-bit FNV-1a tile-hash dirty
> detection, keyboard+pointer injection, clipboard both ways) and
> `kernel/sync.c/.h` (**cloud file sync** LIST/GET/PUT on port 7070 +
> **settings serialize/save/load** to `settings.cfg`). Added
> `keyboard_set_idle_hook` (services backgrounded servers at the text shell),
> `mouse_inject`, `keyboard_inject_char`. 4 shell commands
> (`vnc`/`sync`/`synccfg`/`clipsync`), shell **v30.0 / 126 commands**. All four
> pillars verified live against real clients over QEMU hostfwd (a Python RFB 3.3
> client captured pixel-perfect 1024×768 frames and drove `ls` via injected
> KeyEvents; the sync client did LIST/GET/PUT round-trips; settings round-tripped
> theme=ocean through save→change→load). Passed an adversarial review (2 bugs
> found + fixed: a keyboard-buffer race and `parse_uint`'s ambiguous 0). Full
> writeup in `SESSION_LOG.md`; gotchas in §3 "Phase 45 additions"; files in §6.

The rest of this section is the original Era 5 (Multimedia) narrative.

Entered at Phase 38. Implemented three full phases, each: design → code → build →
drive in QEMU → verify with screenshots + numeric/audio checks → update docs.

### Phase 38 — Sound Driver  ✅
- **`kernel/audio.c/.h`** — device-agnostic core: software **mixer** (up to 8
  PCM voices, per-voice + master volume 0–100, mono→stereo up-mix, Q16
  nearest-neighbour resampling to the device rate), **WAV/RIFF parser** (8/16-bit
  mono/stereo), integer **tone generator** (Bhaskara sine approx + Q16 phase —
  no FPU), and a boot-installed `startup.wav` chime.
- **`kernel/ac97.c/.h`** — real **Intel 82801AA AC'97** driver (QEMU
  `-device AC97`, PCI `8086:2415`): PCI probe, bus-master enable, cold reset,
  codec reset, NAM (mixer) + NABM (bus-master) I/O windows from the two BARs,
  **Buffer Descriptor List DMA** PCM-out, polled to completion (timeout + spin
  cap so it can't hang). Registers itself with the audio core.
- Shell: `sndinfo`, `play <file.wav>`, `volume <0-100>`, `tone <hz> <ms>`,
  `mixer` (3-voice C-E-G chord).
- `build.bat`: link `audio.o`/`ac97.o`; QEMU run/debug lines get
  `-audiodev dsound,id=snd0 -device AC97,audiodev=snd0`.
- **Verified**: codec ready (NAM 0xC000/NABM 0xC500); WAV/tone/mixer all played;
  captured AC'97 output via QEMU `wav` backend → 98.9% non-zero samples,
  peak 28632/32767.

### Phase 39 — Image Formats  ✅
- **`kernel/image.c/.h`** — magic-byte format detect, **BMP** decoder (24/32-bit,
  top-down + bottom-up), decode dispatch, `image_present()` (render one ARGB
  frame scaled-to-fit + caption, no wait), `image_view()` (fullscreen viewer +
  GIF animation), boot-time sample installer. `IMAGE_MAX_DIM 256` cap.
- **`kernel/png.c`** — from-scratch **DEFLATE/zlib inflater** (stored + fixed +
  dynamic Huffman, puff-style canonical decode) + all 5 PNG filters
  (none/sub/up/avg/paeth); 8-bit gray/RGB/palette/gray+alpha/RGBA, non-interlaced.
- **`kernel/jpeg.c`** — **baseline JPEG**: DQT/DHT/SOF0/DRI/SOS, canonical
  Huffman, dequant, integer **IDCT** (NanoJPEG-style), chroma upsampling for any
  sampling factor, YCbCr→RGB; grayscale + 3-component.
- **`kernel/gif.c`** — **GIF87a/89a**: **LZW** (variable width, clear/EOI),
  global/local colour tables, interlace, transparency, multi-frame compositing
  with disposal; `gif_decode_frame()` drives animation.
- **`kernel/sample_images.h`** (generated by `gen_images.py`, Pillow): embedded
  `icon.bmp` / `logo.png` / `photo.jpg` / `anim.gif`, each < 4 KB so they install
  into the RAM-FS at boot.
- Shell: `imginfo` (decode all 4, print format/size/frames/centre pixel),
  `view <file>`.
- `build.bat`: link `image.o`/`png.o`/`gif.o`/`jpeg.o`.
- **Verified**: each decoded centre pixel matches the `gen_images.py` reference —
  BMP `0xF0F0F0`, PNG `0x28283C` (exact), JPEG `0x828282`, GIF `0x181820`
  (frames=3). `view` renders all four correctly (incl. GIF animation).

### Phase 40 — Video Playback  ✅
- **`kernel/video.c/.h`** — **RIFF/AVI parser** (`avi_load`: walks `hdrl`→
  `avih`/`strh`/`strf`, then `movi` collecting `NNdc`/`NNdb` video + `NNwb` audio
  chunk offsets; ignores `idx1`) and an **A/V-synced media player**
  (`video_play`): each Motion-JPEG frame → `jpeg_decode` → `image_present`; PCM
  concatenated and played one frame's worth per video frame via `audio_play_pcm`
  — **audio clock paces video**; tick-paced fallback for silent clips.
- **`kernel/sample_video.h`** (generated by `gen_video.py`): embedded `demo.avi`
  (~57 KB, MJPEG 80×60, 16 frames @ 8 fps, 8 kHz PCM audio). Hand-written AVI
  muxer in the generator.
- Refactor: `image_present()` was extracted from `image_view()` so the viewer and
  player share frame rendering.
- Shell: `vidinfo [file]`, `mplay [file]` (no arg = embedded demo).
- `build.bat`: link `video.o`. `kernel.c`: `video_init()`.
- **Verified**: `vidinfo` → MJPG 80×60, 16 frames, 8 fps, audio 8000 Hz 1ch
  16-bit, 16 chunks. `mplay` played frames 11/16 & 16/16 (ball animating, colours
  cycling). AC'97 `wav` capture → 70.1% non-zero = synced audio played.

### Phase 41 — GPU Acceleration  ✅  *(added 2026-06-11)*
- **`kernel/virtio.c/.h`** — from-scratch **VirtIO 1.0 modern PCI transport**:
  PCI vendor-capability walk (common/notify/ISR/device cfg), MMIO windows
  identity-mapped page-by-page (PCD set), feature negotiation via the 32-bit
  select windows (`VERSION_1` required; no 64-bit math anywhere), **split
  virtqueues** allocated from the identity-mapped heap and driven **polled**
  (2-descriptor chains, notify doorbell, used-ring poll with tick timeout +
  spin cap). INTx is disabled at the PCI level and the ISR acked every poll —
  see gotcha below.
- **`kernel/gpu.c/.h`** — **VirtIO-GPU** driver (QEMU `-device virtio-vga`,
  PCI `1AF4:1050`): controlq (16 entries) → `GET_DISPLAY_INFO` →
  `RESOURCE_CREATE_2D` (`B8G8R8X8` = our XRGB) → `ATTACH_BACKING` **backed by
  `fb_back` itself (zero-copy)** → `SET_SCANOUT`. `fb_flip()` routes to
  `gpu_flip()` (TRANSFER_TO_HOST_2D + RESOURCE_FLUSH) when active;
  `gpu_present(x,y,w,h)` = dirty-rect present; `gpu_fill`/`gpu_blit` =
  clipped `rep stosl`/`rep movsl` primitives. Legacy memcpy path kept as
  `fb_flip_legacy()`. Silent VESA fallback when the device is absent.
- **`kernel/sprite.c/.h`** — sprite engine: 32 ARGB sprites (≤64×64),
  per-pixel integer alpha blend, z-order, clipping, `sprite_paint_ball()`.
- Shell: `gpuinfo`, `gpubench`, `sprites` (now **105 commands**, shell v26.0).
- `build.bat`: link `virtio.o`/`gpu.o`/`sprite.o`; QEMU lines use
  `-vga none -device virtio-vga`.
- **Verified**: login/desktop render through the virtio scanout (VGA compat
  retired); `gpubench` → **4,620 fps full-frame present vs 107 fps VESA memcpy
  (43×)**, 31,777 fps 64×64 dirty rects, 4.1 GB/s fills, 3.1 GB/s blits;
  `sprites` → 10 alpha-blended balls @ ~520 fps avg. `-vga std` fallback
  re-verified. Screenshots: `ph41_*.png`, `screenshots/gpu-{sprites,bench}.png`.
- **Bug-fix confirmed by retest**: pre-fix the kernel froze permanently at
  `gpu_init`'s first virtqueue command (CPU pinned in `irq_handler`); post-fix
  the same image booted to login in ~10 s and the `gpubench` run alone pushed
  ~18,000 virtio commands through the once-frozen poll path with zero hangs.
  The INTx-storm fix is solid, not just masked.

### Phase 42 — Gaming Framework  ✅  *(added 2026-06-12, Era 5 finale)*
- **`kernel/gamepad.c/.h`** — 12-button **virtual game controller** fed by a new
  raw-scancode hook in the PS/2 driver (`keyboard_set_raw_hook` in
  `keyboard.c/.h`): per-key held state (`key_state[256]`, plain + E0-extended),
  held/edge/event-queue APIs, `gamepad_acquire()`/`release()` (acquired = keys
  consumed before console translation). Each button has up to 3 source keys
  (UP = Up/W, A = Ctrl/X, B = Space/Z, START = Enter, SELECT = Esc/Tab…).
- **`kernel/game.c/.h`** — **"NexusSDL"**: 256-color palettized surface (≤
  320×240) with RAMP palette (15 ramps × 16 shades), drawing primitives +
  scaled 8×8 text, **Q16.16 math kit** (`fx_mul` widening `imull`, `fx_div`
  inline `idivl` — no libgcc), LUT trig (Bhaskara, 1024-unit circle),
  tick-locked `game_sync()` (~18 fps), non-blocking PC-speaker `game_sfx`.
  `game_present()` palette-expands + integer-scales to `fb_back`, presents via
  `gpu_present` dirty rect (or `fb_flip` fallback).
- **`kernel/doom.c/.h`** — **NEXUSDOOM**: DDA raycaster (per-column z-buffer),
  4 procedural 64×64 textures + EXIT door, 6 chasing billboard imps (z-tested),
  hitscan pistol, HUD, minimap, title/play/dead/win states. 18 KB texture arena
  on the heap (kept small on purpose — see layout landmine).
- **`kernel/breakout.c/.h`** — ~200-line Breakout (Q16 ball physics, english).
- Shell: `gameinfo`, `gamepad` (visual tester), `doom`, `breakout` — now
  **109 commands**, shell v27.0. `kernel.c`: `gamepad_init()`+`game_init()`
  after `gpu_init()`. `build.bat`: 4 new objects.
- **Bootloader rework (the kernel outgrew real mode)**: at 619 KB the old
  in-place load at 0x10000 hit the VGA hole at 0xA0000 (576 KB cap).
  `boot2.asm` now BIOS-reads 32 KB chunks into a bounce buffer at 0x10000 and
  copies each to **1 MB during brief protected-mode hops** (16-bit GDT entries
  0x18/0x20 for the PM→real return); `linker.ld` base 0x10000 → **0x100000**;
  capacity now ~720 KB (`KERNEL_SECTORS 1440`).
- **Three long-standing console bugs root-caused & fixed** (see §3): the
  white-block artifact (0xB8000 read-back), the 8×8-font off-by-one, and the
  shell history-recall stranding (the "helpS" floating mid-screen — captured
  only the input start *column* and repositioned to a stale row after scroll).
- **Verified live**: boot → login → shell through the new loader; `help` heavy
  scroll clean (no white block); `help`→up-arrow→`S` shows `helps` glued to the
  prompt (history-recall fix); `gamepad` boxes light on held keys
  (HELD/EVENTS/FPS 18); `doom` full loop — killed an imp (2 shots, 100%
  accuracy), died to melee, Esc-exit stats "287 frames in 15s = 18 fps
  (tick-locked)"; `breakout` bricks/serve/lives all good. Screenshots:
  `screenshots/doom{,_title}.png`, `breakout.png`, `gamepad.png`.

---

## 3. Key constraints & gotchas discovered (IMPORTANT for future work)

### Phase 48 additions
- **The PIT is the DEFAULT 18.2 Hz — never reprogrammed for IRQ0** (only
  channel 2 for the speaker). The ENTIRE codebase hardcodes `system_ticks/18 =
  seconds` / `*55 ms` (STATUSBAR_INTERVAL, TICK_MS, TIMESLICE, DHCP=90, DNS=36,
  uptime, game 18fps, audio pacing, Win32 GetTickCount). **Do NOT reprogram the
  PIT** without auditing ~dozens of files — it's a landmine. Consequence:
  timing resolution is ±55 ms, so "boot < 1s" is measured coarsely (boot is ~3
  ticks ≈ 165 ms).
- **The scheduler is intentionally DISABLED — do NOT call `scheduler_start()`.**
  It exists (round-robin, IRQ0-driven, timeslice=5) but is never enabled; the
  kernel is one shared address space with NO locks on heap/VGA/VFS/net rings, so
  enabling preemption would corrupt them mid-syscall AND worsen the boot #DF
  (and context_switch would jump to processes with uninitialized esp). SMP is
  even further out. `smp`/`preempt` document this honestly; keep it that way
  unless someone does the full locking + per-CPU rewrite.
- **The intermittent boot #DF is an IRQ-during-init race** (root cause confirmed
  Phase 48): `idt_load` (idt_asm.asm) does `sti` BEFORE `pic_init` unmasks IRQs
  and the rest of init runs, so IRQ0 fires while pmm/heap/rtc/VGA/net are being
  built; the timer handler's status-bar redraw (reads rtc/pmm/VGA) + `net_poll`
  is the trigger. Phase 48 added a `volatile bool boot_init_done` guard
  (`system_ticks++` stays unconditional; the rest defers until init done) — a
  MITIGATION. The DEFINITIVE fix (deferred) is to move `sti` out of `idt_load`
  and enable interrupts only at the boot_init_done point. The PIC EOI is sent by
  `irq_handler` BEFORE the callback, so an early-return guard is PIC-safe — keep
  that ordering if you touch idt.c.
- **`memcpy`/`memset` are now `rep movsl`/`stosl` (aligned) — overlap rules.**
  A forward `rep movsl` is correct for disjoint copies and for dst<src overlap
  (tcp.c rx shift). For dst>src overlap memcpy falls back to a backward byte
  copy (no current caller hits it). There is NO `memmove`. If you add an
  overlapping copy with dst>src, memcpy already handles it; but prefer adding a
  real `memmove` alias if that pattern spreads. The asm uses `(uint32_t)` casts
  (types.h has no `uintptr_t`), explicit `cld`, `"memory"` clobber, `+D/+S/+c`.
- **Boot-time is measured from timer-online (system_ticks=0 at pic_init) to
  end-of-init**, stamped by `perf_mark_boot_done()` at the boot_init_done point
  (after the last _init, before the blocking `login_run`). It's REAL wall-clock
  (IRQs are on during init) but coarse (±55 ms). Read it via `perf_boot_ticks()`.

### Phase 47 additions
- **`touch` is already the file-create command** (shell.c `cmd_touch` → ramfs).
  Phase 47's touch-MODE command is therefore named `gesture`, not `touch`. Check
  the strcmp dispatch chain for an existing command name before adding one — a
  few obvious names (`touch`, `search`→GUI, `power`→menu) are taken.
- **Gesture recognition is sampling-rate dependent.** `mobile_poll()` is edge
  detection over the mouse button: it must SEE the button held across samples
  then released. A burst of input processed between two polls (e.g. all of a
  VNC press→drag→release handled by one `vnc_poll`) would be missed — so
  `vnc.c inject_pointer()` calls `mobile_poll()` after every `mouse_inject` to
  sample at full event rate. Any future synthetic-input source feeding the
  gesture layer must do the same, or be polled finely enough.
- **`mobile_poll()` runs from THREE same-thread sites** (desktop per-frame,
  shell idle hook, vnc inject_pointer). This is safe ONLY because it's all one
  cooperative thread and `mobile_poll` is never called from an IRQ — each call
  is an atomic read-and-update of the gesture statics. Don't call it from an IRQ
  handler (it reads `mouse_get_state`/`system_ticks`, fine, but the statics
  aren't IRQ-safe against the cooperative callers).
- **Orientation is LOGICAL, not a real display rotation.** `mobile_screen_w/h()`
  swap W/H in portrait, but the framebuffer, all blitters, mouse coords, and the
  taskbar still assume physical 1024×768. Only code that explicitly reads
  `mobile_screen_w/h()` / `mobile_is_portrait()` adapts. A true rotation = a
  big rewrite of every drawing primitive (deferred — don't claim it).
- **Low-power only throttles the IDLE clock redraw.** The desktop loop reads
  `mobile_redraw_interval()` (18 normal / 72 low-power) for the periodic refresh;
  real input (`if(inp)`) and theme/scale changes still force an immediate
  redraw, so interaction stays responsive. It's a redraw/HLT throttle, not CPU
  frequency scaling (there's no ACPI/P-state support).

### Phase 46 additions
- **The shell has NO machine-readable command list.** Command names +
  descriptions live only as hardcoded `vga_print` lines in `cmd_help`
  (shell.c). The AI assistant therefore carries its OWN `kb_entry_t` table +
  intent table in `assistant.c` — they are maintained by hand and must be kept
  in sync when commands change. (If you add a command and want the assistant to
  know it, add a KB row and, if it should be reachable via `ask`, an intent row.)
- **`shell_exec_line(const char*)` is the one public hook to run a command
  programmatically** (copies + tokenizes in place + dispatches; void return, no
  output capture). The assistant runs resolved commands through it. There is no
  return code, so the assistant can't tell if a command "succeeded" — it only
  controls *whether* to run.
- **`cmd_fontsize` REJECTS out-of-range (it does NOT clamp).** An unclamped
  value emits a no-op error, so the assistant clamps NL-extracted numbers to
  1..4 for fontsize (and 0..100 for volume, belt-and-suspenders) BEFORE building
  the command. Real theme names are exactly `dark light retro ocean hicon` — the
  theme arg is whitelisted, not free-text.
- **Destructive NL commands must never auto-run on a weak match.** `rm`, `kill`,
  `chmod`, `firewall` are flagged destructive in the intent table; the assistant
  requires best>=20, margin>=6, AND a non-empty extracted arg before it will run
  one — otherwise it prints the command for the user. `assistant_ask(q, run)`'s
  `run=false` is a hard explain-only override regardless of confidence.
- **string.c has NO ctype/atoi/tokenizer.** The assistant implements `lc`,
  `is_alnum`, a tokenizer, substring/subsequence match, and stopword filtering
  locally. Any future text-processing module needs the same (or factor these
  out). Integer-only throughout — verified `nm assistant.o` has no `__*di3`.
- **Editor autocomplete counts ONLY real insertions.** `insert_char` is a no-op
  once a line hits `EDITOR_MAX_COLS-2`; the Tab-cycle bookkeeping (`ac_inserted`)
  must reflect what actually got inserted, or the next Tab's delete loop corrupts
  the buffer (caught in review). Any future "insert N then undo N" editor feature
  must verify each insert took.

### Phase 45 additions
- **Polled TCP servers must re-listen on client CLOSE, not just on TCB-inactive.**
  First cut of the VNC/sync servers (copied from `rshell`/`httpd`) only re-armed
  the listener when the TCB went `!active`. But a client disconnect leaves the
  TCB in FIN_WAIT (still `active`) for a while, so the *next* connection was
  RST'd by QEMU's NAT — the second VNC/sync client always failed. Fix: in
  `*_poll`, also detect `c->closed || (c->connected && c->state !=
  TCP_ESTABLISHED)`, `tcp_close` our side, and **immediately re-listen**
  (`vnc_relisten`/`sync_relisten`). `rshell`/`httpd` happen to survive this only
  because they're pumped every tick; with sparse polling it bites.
- **Backgrounded network servers need the poll pumped at the TEXT shell too.**
  `vnc_poll`/`sync_poll` run per-frame from `desktop_run`, but the raw text shell
  blocks in `keyboard_getchar()` (spins on `hlt`), so a server started at the
  shell got zero service and every client hung at byte 0. Added a
  `keyboard_set_idle_hook()` slot that `keyboard_getchar` calls each wait
  iteration; `shell_run` installs `shell_idle_pump()` = `net_poll`+`vnc_poll`+
  `sync_poll`. Any future "background service driven from the shell" must rely on
  this hook (or be an interactive blocking command like `vnc serve`).
- **VNC pixel format is zero-copy ONLY because of full-width rects.** The RFB
  PIXEL_FORMAT (32bpp, little-endian, shifts R16/G8/B0) is byte-identical to the
  `fb_back` 0x00RRGGBB, so a full-width (x=0, w=1024) row-band is a contiguous
  `&back[y*1024]` slice handed straight to `tcp_send` — no scratch buffer, no
  per-pixel swap. **Arbitrary-x rectangles would break contiguity** and force a
  per-line scratch copy (heap-layout landmine). Keep VNC rects full-width.
- **`tcp_send` is fine with multi-KB buffers but its `len` is `uint16_t`.** It
  chunks into MSS segments internally and transmits synchronously (QEMU delivers
  during the poll), so you can hand it a whole band — but a single call can't
  exceed 65535 bytes. VNC bands are ≤8 rows × 1024 × 4 = 32768 B; a full 3 MB
  frame MUST be split (it is, by the band loop). Don't compute a whole-frame byte
  total (tempts 64-bit math; no libgcc).
- **Input injection goes through the EXISTING buffers, no new IRQ hooks.** RFB
  KeyEvent → `keyboard_inject_char()` (pushes into the same circular buffer
  `keyboard_getchar` drains; arrows map to 0x80–0x83, NOT the KEY_* scancodes);
  RFB PointerEvent → `mouse_inject()` (sets the shared mouse `state` like a real
  PS/2 packet). RFB button bits are L/M/R; mouse.h masks are L/R/M — remap. These
  run from `vnc_poll` (normal context), not IRQ, so they don't race the IRQ
  producers in practice (single-threaded, cooperative).
- **Settings have no persistence layer — Phase 45 added one in `sync.c`, not
  `settings.c`.** `settings.c` is a UI window only. `settings_serialize/save/load`
  live in `sync.c` and read/write the live state via `theme_get_index`/`theme_set`,
  `font_get_scale`/`font_set_scale`, `accessibility_reader_on`/`set_reader` +
  `contrast_on`/`toggle_contrast`, `wallpaper_get`/`wallpaper_set`. The blob is a
  `key=value` text file `settings.cfg` in the RAM-FS (in-RAM only — lost on
  reboot, like the Phase-44 user DB; persisting to FAT32 is still deferred).

### Phase 44 additions
- **Process "isolation" is COOPERATIVE, not memory isolation.** Every process
  runs in ONE shared 16 MB address space, ring 0, with a single global
  `page_directory` (paging.c) — there is no per-process CR3, no ring-3, the
  `is_user`/`user_stack` fields (Phase 20) are dead, and `vmm_fork()` returns a
  page dir nobody stores or loads. Phase 44 added `process_t.uid` + kill/signal
  permission checks (who may *act on* a process), which is the honest shippable
  slice. REAL isolation = a 2–3 day rewrite (per-process page dirs, CR3 swap in
  context_switch, ring-3 transitions via IRET, per-process TSS ESP0, VMM
  refactor). Don't claim memory isolation until that exists. A ring-3 process
  today could read/write kernel and other-process memory freely.
- **`chmod 000` vs the "uninitialized node" sentinel (bug fixed).** First cut of
  `vfs_check_perm` treated `mode == 0` as "world-accessible" (to spare nodes
  predating the perm system) — but `chmod 000` sets mode to exactly 0, so a
  deliberately-locked file read as world-readable. Lesson: don't overload mode 0
  as a sentinel. Every node now gets a non-zero default from `vfs_init_perms`
  (called in `ramfs_create`), so mode 0 genuinely means "no access for
  non-owners". The same `mode != 0` mistake was in `ramfs_delete` — delete is
  gated purely on ownership now.
- **Permission checks live only at the VFS read/write choke points.** Boot runs
  as root (uid 0 bypasses all checks), so boot-time sample-file installs are
  unaffected — enforcement only bites once a non-root user is logged in. New FS
  ops that bypass `vfs_read`/`vfs_write` (e.g. reading `node->data` directly)
  would skip the check; route file content access through the VFS.
- **Password hashing is teaching-grade, on purpose.** `users.c` uses a salted
  integer-only FNV-1a + 64-round KDF (no 64-bit math / libgcc). CRC32 (pkg.c)
  was deliberately NOT reused — it's a non-cryptographic checksum. Don't upgrade
  to SHA/bcrypt without first adding 64-bit math support or a table-based impl.
- **The firewall hooks are in `ip.c`** — ingress in `ip_handle_packet` (after
  validation, before protocol demux), egress in `ip_send_packet` (before frame
  build). It filters on the packet's *destination* port for both directions
  (the local service / the remote service respectively). It's disabled by
  default; adding a rule via the shell auto-enables it. Keep rule eval O(small)
  — it runs in the net RX path which budgets only a few packets/tick.
- **login_run() now returns a uid and loops until valid.** Its signature changed
  from `void` to `uint32_t` (login.h). `users_init()` MUST run before it
  (kernel.c) or there are no accounts. The shell process's `uid` is set from the
  return value. Default creds: root/root, guest/guest (change via `passwd`).

### Phase 43 additions
- **`beep()` BLOCKS — never call it from an IRQ handler.** `speaker.c` `beep`
  spins on `hlt` until `system_ticks` advances; calling it from the keyboard
  IRQ1 hook would deadlock (ticks are also driven by IRQ). The accessibility
  raw hook therefore only flips state + sets a `pending_event` flag; the actual
  earcon plays from `accessibility_poll()`, which the **desktop/shell main
  loops** call in normal context. Any future IRQ-time audio must defer the same
  way.
- **There is exactly ONE keyboard raw-hook slot** (`keyboard_set_raw_hook` sets
  a single function pointer). Phase 42's gamepad used it; Phase 43 needed it too.
  Resolution: the gamepad's hook body was exported as `gamepad_raw_observe()`,
  and accessibility now **owns** the slot and forwards every scancode to the
  gamepad. Init order matters — accessibility installs AFTER `gamepad_init()`.
  Any third consumer must chain the same way, not call `keyboard_set_raw_hook`
  and clobber the others.
- **The VESA wallpaper ignores `theme->desktop_bg`** — `wallpaper.c` paints
  hardcoded RGB patterns per `current_pattern` in VESA mode (the theme bg only
  drives the text-mode glyph fill). So a new theme's background does NOT change
  the desktop field unless you special-case it. Phase 43 added an explicit
  `theme_get_index()==THEME_HICON` → solid-black branch at the top of
  `wallpaper_draw()`'s VESA path. Remember this if a future theme needs its bg
  to actually show on the desktop.
- **The global font scale must NOT reach fixed character-grid renderers.** The
  raw VESA text console (`vga.c` `vesa_render_char`) and the GUI text-cell
  renderer (`gui.c` `gui_putchar` — Terminal/Notepad windows) position glyphs on
  a fixed `col*FONT_WIDTH`/`row*FONT_HEIGHT` grid. If `font_draw_char` self-scales
  there, a >1× scale draws oversized glyphs on a 1× grid → overlapping garbage
  (found by testing `fontsize 4` at the raw shell — first pass missed it). Fix:
  those two renderers call `font_draw_char_fixed()` (always 1×). The global scale
  is for **free-positioned** label text only (icon labels, window titles, widget
  labels via `font_draw_string`). Scaling grid *content* needs a column-reflow
  rewrite (deferred). Any new fixed-grid text renderer must use `_fixed`.
- **`font_get_active_width/height()` return the UNSCALED base glyph size** even
  with the global accessibility scale active — the renderers use them as the
  base from which they compute scaled output, so folding the scale in there
  would double-apply it. Layout code that wants on-screen pixel extents must
  multiply by `font_get_scale()` itself. `font_measure_string()` DOES include
  the scale (it's a measurement, not a base).
- **The AC'97 `wav` audiodev backend does NOT capture PC-speaker output.** The
  screen reader uses the PIT speaker (port 0x61 / channel 2), a separate device
  from AC'97 — so the `verify-audio-via-qemu-wav-capture` trick can't record
  it. Speaker features are verified *functionally* (the command runs to
  completion = the blocking beep loop executed) rather than by waveform.
- **Pre-existing `appearance.c` theme bug (now fixed)**: the panel called
  `theme_set_by_name()` with display-capitalized names ("Dark") while `theme.c`
  stores lowercase ("dark") and `strcmp` is case-sensitive, so the appearance
  panel's theme selection silently never worked. It now selects by index
  (`theme_set(ap_sel)`; the `theme_names[]` array is index-aligned with
  `themes[]`). If you add a theme, keep `theme.c themes[]`, `theme.h
  THEME_COUNT`, and the two panel mirrors (`appearance.c` `theme_names[]` +
  local `THEME_COUNT`, and `settings.c` which uses `theme_get_name`) in sync.

### Phase 42 additions
- **Legacy VGA memory dies when the virtio scanout activates** (the real cause
  of the "white-block console artifact", now FIXED): once `SET_SCANOUT` lands,
  QEMU unmaps the VGA-compat window at 0xB8000 — **writes are discarded and
  reads return 0xFF**. `vga.c` kept its text model there, so any scroll redraw
  read back 0xFFFF cells (char 0xFF on attr 0xFF = white-on-white) and painted
  the 640×384 text area solid white. The model now lives in a kernel-RAM
  shadow array (`vga_shadow`); 0xB8000 is a write-through target in text mode
  only. **Never read display state back from legacy VGA memory.**
- **`font8x8.h` was off by one from 0x20 up** (now FIXED): the 0x12–0x1F block
  had 13 entries for 14 codes (0x1F ▼ missing), so all 8×8 text rendered as
  char+1 ("UP" → "VQ"). If you add glyphs, count the rows — the array is
  positional with no designated initializers.
- **Console cursor tracking is relative, not absolute** (shell readline fix):
  `vga_backspace` walks back across row/scroll boundaries, but
  `vga_get_cursor_row()` is only meaningful *now* — after any scroll the prompt's
  original row is gone. Never cache a row and reposition to it later. The
  shell's history recall stranded text ("helpS" floating mid-screen) because it
  did exactly that; fixed by reprinting relative to the current cursor
  (`readline_replace`). Any new line-editing code must do the same.
- **VBE scratch buffer moved 0x8000 → 0x0500**: stage2 now extends past 0x8000
  and the old `ModeInfoBlock` buffer silently corrupted the GDT (garbage GDTR,
  boot hang; live memory differed from the on-disk image). Low scratch memory
  for boot2 must come from 0x500–0x7BFF.
- **QEMU `sendkey <key> <hold-ms>` works for held-key gameplay** (qemu_drive.py
  passes it through: `'key:w 2000'`), **but never overlap a long hold with
  more sendkeys** — interleaved holds garble delivery order. Serialize:
  `'key:w 900' sleep:1.5 'key:ctrl 200' sleep:1 …`, sleeps ≥ the hold time.
- **Raw-hook contract**: while a game holds `gamepad_acquire()`, ALL keys are
  consumed (never reach the shell). Anything that loops on `keyboard_getchar`
  must either use the pad API or run before acquire/after release;
  `gamepad_release()` drains stray buffered chars.
- **There are TWO consoles — don't confuse them when reproducing bugs.** (1) The
  raw **VESA text console** (full-screen, no window chrome, `vga.c`/`shell.c`,
  reached by Esc from the desktop — this is where the readline/scroll fixes
  live). (2) The desktop **GUI Terminal app** (a window with its own renderer).
  `clear` at the raw shell does not drop to the GUI; `gui` does. When a user
  screenshot shows full-screen text it's console (1); a titled "Terminal"
  window is (2). A repro that lands in the wrong one tests the wrong code path.
- **`build.bat` can't write `nexus.img` while any QEMU has it open** ("file
  used by another process" → `[FAIL] Image creation`). Quit every QEMU
  instance (monitor `quit`, not kill, to release the lock) before rebuilding.
  Run a private headless instance on a **non-default monitor port** (e.g.
  55556 with a `sed`-patched copy of `qemu_drive.py`/`qmon.py`) so you never
  collide with a window the user launched via `build.bat run`.

### Phase 41 additions
- **Virtio INTx storm (the big Phase 41 bug)**: a polled virtio driver MUST
  keep the device's level-triggered INTx from latching: after the first
  completed request the GPU set ISR=3 and IRQ10 stormed the PIC (handler EOIs,
  line still high → refires), freezing the kernel to a few foreground
  instructions per second. The white-block frozen screen + `info virtio-status`
  (`used_idx=1`, handshake complete) + symbolized stack
  (`gpu_init→gpu_cmd→virtio_run` for minutes) gave it away. Fix (all three in
  `virtio.c`): PCI command bit 10 (INTx disable), `VIRTQ_AVAIL_F_NO_INTERRUPT`
  on the avail ring, and a read of the ISR register (read-to-clear) after
  every poll.
- **virtio-vga's VBE sets mode 0x118 as 24bpp** (pitch 3072), unlike stdvga's
  32bpp. Harmless: `fb_back` is always 32-bit, the GPU scanout bypasses the
  VESA aperture, and the pre-scanout boot console uses the existing 24bpp
  conversion in `fb_flip_legacy`. Don't "fix" the bootloader for this.
- **Debug workflow that cracked it** (reusable): link a symbolized ELF
  alongside the flat binary (`i686-elf-ld -T linker.ld -o kernel.elf <objs>` +
  `nm -n`), then sample `info registers` / `x/Nwx $esp` / `x/Ni $eip` via the
  QEMU monitor socket (`qmon.py`) and map addresses to functions.
  `info virtio-status <path>` / `info virtio-queue-status <path> 0` show the
  device-side handshake/ring state.
- Queue rings + MMIO writes only need **compiler barriers** on x86 (TSO, and
  QEMU completes the request during the doorbell vmexit).

- **Freestanding kernel**: `-ffreestanding -m32 -nostdlib -nostdinc`. No libm, no
  stdlib, and **no libgcc** is linked → avoid 64-bit multiply/divide
  (`__muldi3`/`__udivdi3` are undefined at link). All multimedia code is
  integer-only; sine via Bhaskara approx, JPEG uses an integer IDCT.
- **RAM-FS 4 KB per-file cap** (`RAMFS_MAX_FILE_SIZE` in `ramfs.h`). Anything
  bigger (the demo AVI, larger media) must be **embedded as a C array** in the
  kernel (like `wallpaper_data.h` / `sample_images.h` / `sample_video.h`), not
  installed as a file. Don't raise the cap — it balloons heap use (see next).
- **Heap/stack/framebuffer layout landmine** (pre-existing, see
  `SESSION_LOG.md` "Known issues" + memory `intermittent-boot-doublefault`):
  kernel stack ~`0x400000` and `fb_back` ~`0x500000` sit *inside* the heap range
  (`0x200000`–`0x800000`). **Large `kmalloc`s aggravate a latent
  crash/hang.** Phase 38 originally allocated ~288 KB for the mixer demo and hit
  a lockup; fixed by using small **looping** tone buffers. Keep allocations
  modest; image dims capped at 256².
- **Heap is identity-mapped (<16 MB)** so `kmalloc`/`kmalloc_aligned` addresses
  double as DMA physical addresses (used by AC'97 BDL).
- **Verifying audio/video headlessly**: launch QEMU with the **`wav` audiodev
  backend** (`-audiodev wav,id=snd0,path=out.wav -device AC97,audiodev=snd0`),
  which records the AC'97 output to a file. The header is only finalized on a
  **clean `quit`** via the monitor (not a kill). Then analyze with Python's
  `wave`+`array` for non-zero sample % / peak. (Memory:
  `verify-audio-via-qemu-wav-capture`.)
- **`qemu_drive.py` argument quoting**: only `type:` tokens are typed; a space in
  a command must be inside ONE quoted token, e.g. PowerShell
  `python qemu_drive.py 'type:view logo.png' ret`. Unquoted, the shell splits it
  and the arg is dropped (command falls back to its default file).
- ~~White-block text-console artifact~~ — **FIXED in Phase 42** (it was the
  0xB8000 read-back problem, see the Phase 42 additions above).
- **`hex_to_str` already prepends `0x`** — don't add your own.

---

## 4. Build, run & drive workflow

```powershell
# Build (must run with NexusOS as the working directory)
cd "C:\Users\admin\Desktop\New folder\NexusOS"; .\build.bat            # or: build.bat run

# Toolchain lives under NexusOS\tools\ (i686-elf-gcc, nasm, qemu auto-detected).
# New kernel\*.c files MUST be added to the link list in build.bat (the
# Makefile auto-globs but build.bat does not).
```

Headless drive + screenshot loop (QEMU monitor on TCP 55555):

```powershell
# launch (background) with serial log + monitor (+ optional wav capture)
# (-vga none -device virtio-vga = Phase 41 GPU path; -vga std = VESA fallback)
qemu-system-i386 -m 256 -drive file=nexus.img,format=raw,if=floppy `
  -drive file=disk.img,format=raw,if=ide -boot a -vga none -device virtio-vga `
  -serial file:serial.log -monitor tcp:127.0.0.1:55555,server,nowait `
  -netdev user,id=net0 -device rtl8139,netdev=net0 `
  -audiodev wav,id=snd0,path=out.wav -device AC97,audiodev=snd0

# drive it (PIL required): types via monitor sendkey, screendumps to PNG
python qemu_drive.py sleep:13 type:root ret type:root ret sleep:4 key:esc sleep:2 `
  type:imginfo ret sleep:1 shot:cap
# login accepts any user/pass; Esc on the desktop drops to the text shell.
# clean shutdown (finalizes wav): send `quit` to the monitor socket.

# Phase 45 (Cloud & Sync): add hostfwd for VNC (5900) + sync (7070) to -netdev:
#   -netdev user,id=net0,hostfwd=tcp::5900-:5900,hostfwd=tcp::7070-:7070
# then on the guest `vnc` (background) or `vnc serve` (blocking), `sync serve`,
# and from the HOST:
python vnc_capture.py 127.0.0.1 5900 out.png         # RFB 3.3 client -> PNG
python vnc_capture.py 127.0.0.1 5900 o.png --clip TXT --key x  # clipboard/key in
python sync_client.py 127.0.0.1 7070 list            # cloud file sync
python sync_client.py 127.0.0.1 7070 get readme.txt
python sync_client.py 127.0.0.1 7070 put name.txt "content"
# NOTE: vnc_capture.py reads the REAL fb_back over RFB — more reliable than the
# monitor `screendump` (which can capture a stale frame during a serve loop).
```

Generators (re-run if you change the samples):
`python gen_images.py` → `kernel/sample_images.h`;
`python gen_video.py` → `kernel/sample_video.h`.

---

## 5. Current repo state

- **Branch**: `main`. Remote `origin` = https://github.com/rajvveer/nexusOs.
- **Uncommitted** (staged in the working tree, NOT committed): Phase 37
  (macho/cocoa) + UI polish from a prior session, **Phases 38, 39, 40** from the
  multimedia session, **Phase 41** (virtio/gpu/sprite, 2026-06-11), and
  **Phase 42** (gamepad/game/doom/breakout + bootloader high-load +
  vga/font8x8/shell-readline fixes, 2026-06-12), **Phase 43** (accessibility:
  `kernel/accessibility.c/.h` + theme/font/wallpaper/gamepad/desktop/shell
  changes, 2026-06-14), **Phase 44** (security: `kernel/users.c/.h` +
  `kernel/firewall.c/.h` + login/vfs/ramfs/process/ip/shell changes,
  2026-06-14), **Phase 45** (cloud & sync: `kernel/vnc.c/.h` + `kernel/sync.c/.h`
  + keyboard/mouse inject hooks + desktop/shell poll wiring, 2026-06-14),
  **Phase 46** (AI assistant: `kernel/assistant.c/.h` + editor Tab autocomplete +
  shell/kernel wiring, 2026-06-15), **Phase 47** (mobile/embedded:
  `kernel/mobile.c/.h` + desktop/shell/vnc wiring, 2026-06-15), **Phase 48**
  (performance: `kernel/perf.c/.h` + optimized `string.c` + kernel.c boot
  guard/timing, 2026-06-15), plus
  README/SESSION_LOG/screenshots updates and the
  `gen_images.py`/`gen_video.py` tools. The user has not asked to commit; ask
  before committing. Last actual commits were `7236194` (README screenshots)
  and `aacd2d9` (Phases 34–36) — HEAD is still `7236194`, everything above is
  uncommitted working-tree state.
- Build is green (no new warnings from the Phase 48/47/46/45/44/43/42 files; the
  only `shell.c` warnings are pre-existing — `env_get`/`env_set` implicit decls,
  pointer-from-integer at lines ~362/1107/1141/2286, a couple of
  misleading-indentation lines). Kernel ≈ 687 KB (high-loaded at 1 MB, ceiling
  ~720 KB — only ~33 KB of headroom left; getting tight — see §7). `nexus.img`
  is padded to the 1.44 MB floppy size.
- **Boot is now more reliable**: the Phase-48 `boot_init_done` IRQ guard
  mitigates the intermittent boot #DF — no `!EX:` fault across the many reboots
  of the Phase-48 verification session.
- **Phase 45 went through an adversarial code review** (21 findings → 2 confirmed,
  both fixed): a `keyboard_inject_char` vs IRQ1 race on the ring-buffer head
  (now EFLAGS-guarded pushf/cli/popf around the task-side put), and `parse_uint`
  not distinguishing 0 from a parse error (now returns -1 on non-numeric, so a
  malformed `PUT name xyz` is rejected with `ERR size` instead of silently
  creating an empty file). Both re-verified live.
- **All three console fixes verified on a fresh rebuild** (the user's earlier
  "helpS" screenshot was from an instance that has since closed; the current
  `nexus.img` is correct — relaunch via `build.bat run` to see it).
- Lots of untracked scratch screenshots (`ph36_*`, `ph38_*`–`ph41_*` PNGs/PPMs)
  and the `qmon.py` debug helper accumulate in the NexusOS dir; this Phase-45
  session's scratch (`p45_*.png`, `*.ppm`) was cleaned up after copying the
  keepers. The canonical screenshots live in `screenshots/` (latest additions:
  `cloud-vnc-desktop.png`, `cloud-sync-files.png`, `cloud-vnc-keyinject.png`).
  The host-side test tools `vnc_capture.py` (RFB 3.3 client) and
  `sync_client.py` (sync protocol) are kept for re-verification.

---

## 6. Files added/changed this session

New: `kernel/audio.c/.h`, `kernel/ac97.c/.h`, `kernel/image.c/.h`,
`kernel/png.c`, `kernel/jpeg.c`, `kernel/gif.c`, `kernel/sample_images.h`,
`kernel/video.c/.h`, `kernel/sample_video.h`, `gen_images.py`, `gen_video.py`,
**`kernel/virtio.c/.h`, `kernel/gpu.c/.h`, `kernel/sprite.c/.h`, `qmon.py`
(P41)**, **`kernel/gamepad.c/.h`, `kernel/game.c/.h`, `kernel/doom.c/.h`,
`kernel/breakout.c/.h` (P42)**, this file.
Changed: `kernel/kernel.c` (init calls for `audio/ac97/image/video/gpu` +
`gamepad/game`), `kernel/shell.c` (19 new commands + help + includes; header now
"v27.0 / Phase 42 / 109 commands"; **`shell_readline` history-recall rewritten
to a relative `readline_replace` — fixes the "helpS" stranding**),
`kernel/framebuffer.c/.h` (`fb_flip` → GPU routing + `fb_flip_legacy`),
`kernel/keyboard.c/.h` (raw-scancode hook, P42), `kernel/vga.c` (RAM shadow
text model, P42 fix), `kernel/font8x8.h` (missing 0x1F glyph, P42 fix),
`kernel/doom.c` (session-scoped exit stats), `boot/boot2.asm` + `linker.ld`
(chunked high load to 1 MB, P42), `build.bat` (link list + QEMU `-vga none
-device virtio-vga` + audio device), `README.md`, `SESSION_LOG.md`.

Shell commands added (19): `sndinfo play volume tone mixer` (P38),
`imginfo view` (P39), `vidinfo mplay` (P40), `gpuinfo gpubench sprites` (P41),
`gameinfo gamepad doom breakout` (P42). Total now **109**.

### Phase 43 additions (2026-06-14)
New: **`kernel/accessibility.c/.h`** (the accessibility subsystem),
`screenshots/accessibility-highcontrast.png`, `accessibility-accinfo.png`.
Changed: `kernel/theme.c/.h` (5th `"hicon"` theme, `THEME_HICON`/`THEME_COUNT
5`), `kernel/wallpaper.c` (black-field override when hicon active),
`kernel/font.c/.h` (`font_set_scale`/`font_get_scale` + global-scale honoring
in the non-scaled draw/measure helpers), `kernel/gfx.c/.h`
(`gfx_set_font_scale` wrapper), `kernel/gamepad.c/.h` (hook refactored to
exported `gamepad_raw_observe`), `kernel/keyboard.*` (unchanged — already had
the single raw-hook slot), `kernel/kernel.c` (`accessibility_init()` after
`speaker_init`), `kernel/desktop.c` (per-frame `accessibility_poll()` +
theme/scale-change redraw), `kernel/shell.c` (5 commands + Access help row +
header **v28.0 / Phase 43 / 114 commands** + help banner `v18.0`→`v28.0` +
`theme` help lists `hicon`), `kernel/appearance.c` (theme panel: 5 themes +
**case-mismatch bug fixed** → `theme_set` by index), `build.bat`
(`accessibility.o`), `README.md`, `SESSION_LOG.md`, this file.
Shell commands added (5): `accinfo fontsize contrast reader say`. Total now
**114**.

### Phase 44 additions (2026-06-14)
New: **`kernel/users.c/.h`** (accounts + salted-hash auth + current user),
**`kernel/firewall.c/.h`** (packet filter), `screenshots/security-file-perms.png`,
`security-firewall.png`.
Changed: `kernel/login.c/.h` (real auth loop, returns uid),
`kernel/lockscreen.c` (verify against logged-in user's hash), `kernel/vfs.c/.h`
(uid/gid/mode on fs_node_t + perm checks at read/write choke points +
chmod/chown/mode-string), `kernel/ramfs.c` (default perms on create + ownership
check on delete), `kernel/process.c/.h` (process uid + `process_terminate_as`),
`kernel/ip.c` (firewall ingress/egress hooks), `kernel/kernel.c`
(`users_init` before login, `firewall_init` after net, shell uid from login),
`kernel/shell.c` (8 commands + Security help row + `whoami`/`kill`/`rm`/`ls -l`
upgrades + header **v29.0 / Phase 44 / 122 commands**), `build.bat`
(`users.o`/`firewall.o`), `README.md`, `SESSION_LOG.md`, this file.
Shell commands added (8): `id users passwd useradd userdel chmod chown
firewall`. Total now **122**.

### Phase 45 additions (2026-06-14)
New: **`kernel/vnc.c/.h`** (RFB 3.3 VNC remote-desktop server),
**`kernel/sync.c/.h`** (cloud file sync + settings serialize/load), host-side
test tools `vnc_capture.py` + `sync_client.py` (not linked),
`screenshots/cloud-vnc-desktop.png`, `cloud-sync-files.png`,
`cloud-vnc-keyinject.png`.
Changed: `kernel/mouse.c/.h` (`mouse_inject` absolute-pointer hook),
`kernel/keyboard.c/.h` (`keyboard_inject_char` + `keyboard_set_idle_hook`,
EFLAGS-guarded inject), `kernel/kernel.c` (`vnc_init`/`sync_init` after
`rshell_init` + includes), `kernel/desktop.c` (per-frame
`net_poll`/`vnc_poll`/`sync_poll` + includes), `kernel/shell.c` (4 commands +
Cloud help row + `shell_idle_pump` idle hook + header **v30.0 / Phase 45 / 126
commands** + welcome banner `v20.0`→`v30.0` + includes), `build.bat`
(`vnc.o`/`sync.o` + hostfwd 5900/7070), `README.md`, `SESSION_LOG.md`, this file.
Shell commands added (4): `vnc sync synccfg clipsync`. Total now **126**.

### Phase 46 additions (2026-06-15)
New: **`kernel/assistant.c/.h`** (offline NL->command matcher + command
knowledge base + smart file search + autocomplete provider),
`screenshots/ai-ask-nl-command.png`, `ai-smart-find.png`,
`ai-editor-autocomplete.png`, `ai-destructive-guard.png`. Host test tools
reused from Phase 45 (`vnc_capture.py` for screenshots).
Changed: `kernel/editor.c` (`#include assistant.h`, `autocomplete()` helper +
Tab case + footer hint + the ac_inserted full-line fix), `kernel/kernel.c`
(`assistant_init()` after `users_init` + include), `kernel/shell.c` (3 commands
`ask`/`ai`/`find` + `shell_join_args` + AI help row + header **v31.0 / Phase 46
/ 129 commands** + welcome banner v30->v31 + include), `build.bat`
(`assistant.o`), `README.md`, `SESSION_LOG.md`, this file.
Shell commands added (3): `ask ai find`. Total now **129**.

### Phase 47 additions (2026-06-15)
New: **`kernel/mobile.c/.h`** (touch-gesture recognizer + logical orientation +
low-power redraw throttle + honest ARM status), `screenshots/mobile-gesture-swipe.png`,
`mobile-orientation-power.png`, `mobile-info-arm.png`.
Changed: `kernel/kernel.c` (`mobile_init()` after `assistant_init` + include),
`kernel/desktop.c` (`mobile_poll()` per frame + `mobile_redraw_interval()` for
the idle cadence + include), `kernel/shell.c` (5 commands `gesture`/`orientation`/
`lowpower`/`arm`/`mobileinfo` + Mobile help row + `mobile_poll` in
`shell_idle_pump` + header **v32.0 / Phase 47 / 134 commands** + welcome
v31->v32 + include), `kernel/vnc.c` (`inject_pointer` samples `mobile_poll` +
include), `build.bat` (`mobile.o`), `README.md`, `SESSION_LOG.md`, this file.
Shell commands added (5): `gesture orientation lowpower arm mobileinfo`. Total
now **134**.

### Phase 48 additions (2026-06-15) — Era 6 finale
New: **`kernel/perf.c/.h`** (boot-time stamp + benchmarks + honest SMP/preempt
status), `screenshots/perf-benchmark.png`.
Changed: `kernel/string.c` (rep-string `memcpy`/`memset` — `rep movsl`/`stosl`
aligned fast paths + overlap guard; same signatures, no header change),
`kernel/kernel.c` (`volatile bool boot_init_done` flag + timer_callback early-
return guard + `perf_mark_boot_done()`/flag set after the last _init + Boot:
ticks print + include), `kernel/shell.c` (3 commands `perf`/`smp`/`preempt` +
Perf help row + header **v33.0 / Phase 48 / 137 commands** + welcome v32->v33 +
include), `build.bat` (`perf.o`), `README.md`, `SESSION_LOG.md`, this file.
**Deliberately NOT changed** (high-risk landmines): `scheduler.c` (scheduler
stays disabled), `idt_asm.asm` (the `sti` root cause — guard mitigates instead),
the memory layout / KERNEL_SECTORS (the boot-fault overlap).
Shell commands added (3): `perf smp preempt`. Total now **137**.

---

## 7. Next steps

- **Phase 43 — Accessibility ✅ DONE** (this session, 2026-06-14): see
  `SESSION_LOG.md` for the full writeup. New `kernel/accessibility.c/h`
  (PC-speaker screen reader with earcons + pitch spell-out, high-contrast
  `"hicon"` theme + black-field wallpaper override, global 1×–4× font scale,
  Alt+Shift global hotkeys via a raw-hook chained to the gamepad). 5 shell
  commands (`accinfo`/`fontsize`/`contrast`/`reader`/`say`), shell **v28.0 /
  114 commands**. Also fixed a pre-existing `appearance.c` theme-panel bug
  (case-mismatched `theme_set_by_name` → now `theme_set` by index).
- **Phase 44 — Security ✅ DONE** (this session, 2026-06-14): see `SESSION_LOG.md`.
  New `users.c/h` (salted-hash auth, root/guest, current user) + `firewall.c/h`
  (packet filter). Real login auth, rwx file perms + chmod/chown/ls -l,
  cooperative process uid + kill checks, IP-layer firewall. 8 shell commands,
  shell **v29.0 / 122 commands**. Isolation is **cooperative only** (same address
  space — see §3); true memory isolation deferred.
- **Phase 45 — Cloud & Sync ✅ DONE** (this session, 2026-06-14): see
  `SESSION_LOG.md`. New `vnc.c/h` (from-scratch **RFB 3.3 VNC remote-desktop
  server**, port 5900: zero-copy full-width Raw row-bands of `fb_back`, 32-bit
  tile-hash dirty detection, keyboard+pointer injection, clipboard both ways) +
  `sync.c/h` (**cloud file-sync** LIST/GET/PUT protocol on port 7070 +
  **settings serialize/save/load** to `settings.cfg`). 4 shell commands
  (`vnc`/`sync`/`synccfg`/`clipsync`), shell **v30.0 / 126 commands**. All four
  pillars verified live against real clients (Python RFB client + sync client
  over QEMU hostfwd). Key new infra: `keyboard_set_idle_hook` (pumps backgrounded
  servers at the text shell), `mouse_inject`, `keyboard_inject_char`. **Bug
  fixed**: servers must re-listen on client *close*, not just TCB-inactive (see
  §3). **Deferred**: real internet sync (QEMU user-net needs host port-forwards
  — demo is loopback/hostfwd); `settings.cfg` is in-RAM only (persist to FAT32
  with the Phase-44 user DB); VNC sends one coalesced full-width band rather than
  per-tile rects (simpler, still bandwidth-bounded by the pull model + caps);
  hardware-cursor via the virtio-gpu cursorq still unused.
- **Phase 46 — AI Assistant ✅ DONE** (this session, 2026-06-15): see
  `SESSION_LOG.md`. New `assistant.c/h` — offline rule-based `ask` (NL->command,
  runs via `shell_exec_line`, destructive guard), `ai` (command KB), `find`
  (smart name+content search), editor Tab autocomplete. 3 commands, shell
  **v31.0 / 129 commands**. Deferred (flagged): the assistant's KB/intent tables
  are hand-maintained (no machine-readable command list exists — see §3);
  `notepad` GUI autocomplete (only the full-screen `editor` got it); a pending
  "did you mean / confirm" slot for destructive commands (today it just prints
  the command, no one-shot yes); broader intent coverage / synonyms.
- **Phase 47 — Mobile/Embedded Mode ✅ DONE** (this session, 2026-06-15): see
  `SESSION_LOG.md`. New `mobile.c/h` — mouse->touch gesture layer (tap/long-press/
  drag/swipe), logical portrait/landscape orientation, low-power idle-redraw
  throttle, honest ARM status. 5 commands, shell **v32.0 / 134 commands**.
  Deferred (flagged): a TRUE display rotation (every blitter assumes width=1024 —
  big rewrite; orientation is logical-only today); making the taskbar/desktop
  actually re-lay-out for portrait (only code that reads `mobile_screen_w/h`
  adapts now); real CPU power management (ACPI/P-states — today low-power is a
  redraw/HLT throttle only); an actual ARM cross-build (needs a toolchain + HAL
  rewrite — `arm` documents it).
- **Phase 48 — Performance Optimization ✅ DONE** (this session, 2026-06-15;
  Era 6 finale): see `SESSION_LOG.md`. New `perf.c/h` — rep-string memcpy/memset
  (~3 GB/s), boot-time measurement (~165 ms, under 1s), a boot-init IRQ guard
  (mitigates the boot #DF), `perf` benchmark, honest `smp`/`preempt` status.
  3 commands, shell **v33.0 / 137 commands**. Deferred (honestly flagged): true
  SMP + preemption (negative-EV rewrites — documented via `smp`/`preempt`);
  reprogramming the PIT (breaks `ticks/18`); the definitive boot-fault fix (move
  `sti` out of `idt_load`); raising the size ceiling / fixing the heap-stack-fb
  layout overlap (still the binding constraint — see below).
- **Phase 49 — App Store & Ecosystem** (next, Era 7 "World Domination" opener):
  NexusOS App Store, developer SDK + docs, app sandboxing, automatic updates.
  Reality check: there's no internet App Store reachable offline, so model it on
  the existing **package manager** (`pkg.c` — `.npk` format, dep resolution,
  bundled repo) — an "app store" UI/CLI over `npkg` (browse/install/update from
  the bundled repo), a developer SDK doc + a sample app template, and "app
  sandboxing" scoped honestly (the kernel is one shared address space, ring 0,
  cooperative — real sandboxing needs the Phase-44 deferred address-space
  isolation; today "sandbox" can only be cooperative uid/perm scoping like
  Phase 44). "Automatic updates" = a version-check over the existing sync/pkg
  channel. **Kernel size is the binding constraint** (~33 KB headroom at 687 KB)
  — Phase 49/50 likely needs the heap/stack/`fb_back` layout cleanup + a
  KERNEL_SECTORS bump (boot2.asm, currently 1440=720KB) to raise the ceiling;
  the Phase-48 boot guard makes that layout work safer but doesn't do it.
- **Phase 44 follow-ups deferred** (flagged): true address-space isolation
  (per-process page dirs + ring-3 — the big lift in §3); persisting users/perms
  to disk (the FAT32 volume) so they survive reboot (today the user DB and file
  modes are in-RAM only); group permissions (gid exists on nodes but the check
  folds group into owner); a stateful/connection-tracking firewall (today it's
  stateless first-match).
- Accessibility follow-ups deferred (flagged, not blocking): per-widget
  keyboard focus rings (only `ui_textinput_t` has a focus visual today; buttons
  /checkboxes are click-only — see the recon notes); a true Tab focus chain
  *within* a window (today Tab opens the window switcher); wiring `acc_event()`
  earcons into the desktop event loop (focus move / open / close) now that the
  reader exists; capturing PC-speaker audio headlessly (the AC'97 `wav` backend
  does **not** record the PIT speaker — speaker verification is functional, not
  waveform, today).
- Possible cleanups (out of scope so far, flagged): fix the heap/stack/`fb_back`
  layout so large allocations are safe (would remove the 256² image cap and let
  bigger media play — note the kernel itself no longer lives at 0x10000, so the
  region map in `linker.ld`/`heap.h` is the place to start); a `view`/`mplay`
  that streams from FAT32 (no 4 KB cap); a hardware cursor via the virtio-gpu
  cursorq (queue 1, currently unused); doom could use `game_text_big` for an
  even chunkier title.

---

## 8. Memory pointers (auto-memory at `~/.claude/.../memory/`)

- `nexusos-boot-and-test` — build/run/drive workflow.
- `intermittent-boot-doublefault` — the latent layout crash (don't blame new code).
- `verify-audio-via-qemu-wav-capture` — headless sound/video verification.

*End of session context.*
