# NexusOS — Handoff for the next engineer/agent (written 2026-10-09 22:00 IST)

This file is the single place to start from. It describes what NexusOS is, what was built in
the last two days (Phases 54-57), the exact state of the working tree (NOTHING is committed),
how to build and test headlessly, how to finish and merge the three in-flight work streams,
and the landmines that cost us time. Everything referenced lives inside this repo or in
`C:\nexustest\` on this machine.

## 0. TL;DR

- Repo: `C:\Users\admin\Desktop\New folder` (git root), project in `NexusOS/`. Branch
  `perf-1080p-modern-ui`, last commit `70871d6`. **134 modified + ~45 new files are UNCOMMITTED**
  (Phases 54-57 work). The owner never asked for a commit; ask before committing.
- `NexusOS/` builds clean right now: `build.bat` → `kernel.bin` 878,956 B (0 warnings, cap now
  1,441,792), `nexus.img` (floppy), `nexus.iso`, `disk.img` (16 MiB IDE: FAT32 + NPFS + persist).
- Phases 54, 55, 56 and the "memory foundation" part of 57 are merged and verified.
- Three Phase 57 work streams are UNFINISHED and sit in `NexusOS/handoff/ws_*` as full source
  trees (not merged): **TLS 1.3/HTTPS + reliable TCP** (code complete, tests partly run),
  **graphical browser** (code complete, 5 test runs, one loading bug under investigation),
  **userland/libc/apps** (early: kernel syscall/VT files written, libc barely started).
  Section 4 says exactly how to finish and merge them.
- The owner's goal: "all apps should run on it, like Chrome". Agreed realistic direction:
  real internet (HTTPS + browser) + NexusOS's own POSIX-ish userland so real C programs
  (Lua, kilo, games, coreutils) compile and run. A 64-bit port was explicitly deferred.

## 1. What NexusOS is

Bare-metal x86 (i386) hobby OS in C + NASM: 2-stage bootloader (`boot/boot.asm`, `boot/boot2.asm`
chunk-copies the kernel to 1 MB and enters protected mode, VESA 1920x1080x32), kernel at
`kernel/` (~300 files, single cooperative ring-0 thread for kernel+desktop+built-in apps),
paging, GDT/IDT/PIC, int 0x80 syscalls, ring-3 user processes with their own page directories
(`uproc.c`, loaders `elf.c/pe.c/macho.c`), round-robin scheduler that preempts ONLY ring-3
processes, macOS-style "Aurora" desktop (`macui.c`, `menubar.c`, `dock.c`, `spotlight.c`,
`window.c`, `desktop.c`), ~30 built-in apps, text shell (`shell.c`, commands dispatched in
`execute_command()`), TCP/IP over RTL8139 (`net.c`, `ip.c`, `tcp.c`, `dns.c`, `dhcp.c`), servers
(`rshell.c` 2323, `sync.c` 7070, `vnc.c` 5900, `httpd.c` 8080), ATA PIO + FAT32 + NPFS journaling
FS + RAM filesystem with directories (`ramfs.c`) persisted to disk (`persist.c`), USB
(xHCI/EHCI/OHCI/UHCI + hubs, `usb*.c`), AC97 audio, image/video codecs, kguard crash
containment (`kguard.c`). README.md is long and mostly accurate (sections "What's new",
"Known limitations" were updated through Phase 55).

## 2. What was done in this session (chronological)

| Phase | Content | Status |
|---|---|---|
| Aurora (5.1) | macOS-style desktop, fonts, login, apps' inner UI | merged, verified |
| 54 | per-process address spaces + ring-3 `runelf`, real UHCI USB, ramfs directories, rshell/sync login | merged, verified |
| 55 | kguard crash containment (W^X kernel, NULL page unmapped, stack guard band, #DF task gate), full USB stack (xHCI/EHCI/OHCI/UHCI, hubs, hot-plug, HID F-keys), VNC RFB DES auth (`vncpasswd`), real httpd (`/www`, `/fs/` Basic auth, PUT), TCP TIME_WAIT leak fix, persistent ramfs (A/B slots on disk) | merged, verified (campaign 66/66, net 57/57, USB matrix, bootloops) |
| 56 | "2 fps mouse lag" root cause = QEMU **GTK** display backend on Windows (host repaint 2-4 fps while guest presented 49/s) → `build.bat run/debug` use `-display sdl`; plus dock/menu-bar hover band fast path in `desktop.c` (no full 1080p compose per pointer step) | merged, verified |
| 57 mem | heap 1 MB → 4 MB at 0x400000; ramfs file cap 4 KB → 256 KB; kernel image cap 1,015,808 → 1,441,792 (boot2 `KERNEL_SECTORS 2816`, floppy+ISO boot proven with a 1.39 MB image); persist slots 2 x 1.5 MiB with "too large" skip; `/home/<u>/Downloads`; `sum`, `mkfile` | **merged 21:44 today, build clean; only its own worktree tests ran (10/10 bootloop etc.) — re-run a bootloop + campaign on the merged tree once** |
| 57 tls | reliable TCP + streaming HTTP (`netstream.c`) + TLS 1.3 client + `wget/curl/tlsinfo` | in `handoff/ws_tls`, NOT merged |
| 57 browser | graphical "Nexus Browser" (`webengine.c`, new `browser.c`) | in `handoff/ws_browser`, NOT merged |
| 57 apps | POSIX-ish syscalls, VT100 terminal, libc, Lua/kilo/games ports, App Store entries | in `handoff/ws_apps`, early, NOT merged |

Detailed notes per phase (what was learned, constraints, measured numbers) are in
`handoff/notes/*.md` — read `phase55-kernel-usb-net-persist.md`, `phase57-in-progress.md`,
`qemu-gtk-display-lag.md`, `timing-scheduler-boot-constraints.md`, `nexusos-1080p-memory-layout.md`.

## 3. Build, run, test (Windows host; everything headless-capable)

- Toolchain is vendored: `NexusOS/tools/` (nasm, i686-elf-gcc 7.1, binutils). QEMU 9.1 at
  `C:\Program Files\qemu\qemu-system-i386.exe`. Python 3 with Pillow on PATH.
- Build: open a shell **with cwd = NexusOS** and run `build.bat` (from Git-Bash:
  `cd NexusOS && cmd.exe //c "C:\\Users\\admin\\Desktop\\New folder\\NexusOS\\build.bat"`;
  from PowerShell: `& ".\build.bat"`). ~15 s. `build.bat run` launches QEMU with the right flags
  (`-vga none -device virtio-vga -display sdl`, RTL8139 user-net = real internet, AC97).
  Rules: 0 warnings (`-Wall -Wextra`), size guard 1,441,792, every new `kernel\*.c` must be added
  to the explicit link list in build.bat (Makefile auto-globs; it has a `HOT_OBJECTS` -O2 list),
  hot/crypto/layout files go in build.bat's second (-O2) compile list, no libgcc (no 64-bit
  division/modulo — `__divdi3` is undefined; no floating point in the kernel).
- Headless driving: `handoff/tools/aurora_usb.py` (generic driver: boot, `type:`/`ret`/`key:`/
  `mouse:`/`click`/`shot:` steps, serial crash check), `fullcheck.py` (66-command shell campaign
  in batches), `bootloop.py` (N boots, greps serial for `!EX:`), `harness_usb.py`
  (`USBHC=xhci|ehci|ehci-companion|ohci|uhci`, `USBDEV="kbd,hub(mouse,storage)"`, `mon:` monitor
  steps), `harness_net.py` + `nettests.py` (host-side socket tests over `HOSTFWD="hp:gp,..."`,
  `run:<func>` steps; 57 checks), `harness_persist.py` (`reboot`, `KEEPDISK=1`, `NODISK=1`),
  `harness2.py`/`bootloop2.py` (mem agent: `BOOT=iso`, `grep:` step), `usbmatrix.py`,
  `fpsrun.py`/`hostfps.py` (guest vs host frame-rate probes), `crop.py` (crops the console text
  area at 640,354 size 640x400 from a 1080p screenshot). Typical script:
  `sleep:9;type:root;ret;sleep:1;type:root;ret;sleep:3;key:f12;sleep:2;type:<cmd>;ret;sleep:1;shot:x;serial`
  (login root/root or guest/guest; **F12** leaves the desktop for the text console, Esc does not).
- Serial markers: `!EX:<int> <err> <eip>` = fatal exception (red bar). `!KR:...` = fault
  contained by kguard (command/app recovered) — **treat any `!KR:` during tests as a bug**.
  `PERSIST:` / `USB:` / `[NET]` lines are normal diagnostics.
- Harness gotchas (each cost an hour): use work dirs WITHOUT spaces (`C:\nexustest\<tag>`) — QEMU
  `screendump` and `-drive file=` both break on the space in "New folder"; launch long runs
  detached (`Start-Process ... -RedirectStandardOutput`), Python stdout is buffered until exit;
  windows created from a hidden shell start minimized; Bash heredocs mangle backslashes in
  source files (write files with a proper editor/tool); GUI-launcher commands (`gui calc files
  settings notepad ...`) leave the console — send F12 afterwards; the first key after a
  fullscreen app exits is dropped; with a USB keyboard attached QEMU routes `sendkey` to it.
- Test artifacts of everything run so far: `C:\nexustest\` (`final`, `merge2`, `usb2`, `net`,
  `persist`, `kern`, `lag`, `fps`, `dock`, `mem`, `tls`, `browser`, `apps`).

## 4. The three unfinished work streams — state and how to finish

All three were developed in isolated copies of the repo taken at snapshot **base3** (= the repo
right after Phase 56, before the mem merge). `handoff/base3/NexusOS` is that snapshot (sources
only). `handoff/ws_tls`, `handoff/ws_browser`, `handoff/ws_apps` are the three trees (sources +
their test scripts next to `NexusOS/`). Merge recipe (3-way, base3 as ancestor):

```
python -I NexusOS/handoff/tools/mergeplan.py NexusOS/handoff/base3/NexusOS NexusOS NexusOS/handoff/ws_tls/NexusOS          # dry run: NEW / SINGLE / MULTI
python -I NexusOS/handoff/tools/mergeplan.py NexusOS/handoff/base3/NexusOS NexusOS NexusOS/handoff/ws_tls/NexusOS --apply  # copies NEW/SINGLE, git merge-file for MULTI, reports conflicts
```
(`mergeplan.py` skips `.o/.bin/.img/.png/.log` etc.; after `--apply`, grep for `<<<<<<<` in
build.bat, Makefile, kernel/shell.c, kernel/desktop.c, kernel/macui.c — those are the shared
files — resolve by keeping both sides, rebuild.) The real tree already contains the mem merge,
so every file the mem agent touched (heap/memory/paging/ramfs/persist/shell/build.bat/...)
will be MULTI if a work stream also touched it; conflicts so far have always been trivial
keep-both blocks (each agent wrapped its edits in `/* ---- Phase 5x: ... ---- */` blocks).

### 4.1 ws_tls — reliable TCP + streaming HTTP + TLS 1.3 (`handoff/ws_tls`)
- Files: NEW `kernel/bignum.c/h` (big integers, modexp), `crypto.c/h` (SHA-256, HMAC, HKDF,
  AES-128-GCM, X25519, P-256 ECDSA verify), `cryptokat.h` (known-answer tests, `cryptotest`
  command), `x509.c/h` (DER/X.509, chain validation), `roots.h` (built-in root store, generated
  by `gen_roots.py`), `tls.c/h` (TLS 1.3 client), `netstream.c` (implements
  `kernel/netstream.h` — the shared contract also used by the browser), `netcmd.c/h`
  (`wget`/`curl`/`tlsinfo`); MODIFIED `tcp.c/h` (retransmission, peer-window flow control, 16 KB
  rx window, out-of-order drop + dup-ACK), `net.c/h`, `http.c/h` (`http_get` → wrapper over
  `ns_fetch`), `shell.c`, `build.bat`, `Makefile`. Worktree build: 960,876 B, 0 warnings.
- Tests present: host servers `tlsserver.py` (HTTP 18101, TLS-EC 18102, TLS-RSA 18103, bad cert
  18104; pages 1k/100k/2m/chunked/slow/redirect with CRCs), `tlscheck.py`, `campaign.py` +
  `harness57.py` (full VM campaign: cryptotest KAT, host http/https, real sites, 2 MB throughput,
  20x curl TCB-leak check, nettests 57/57 regression, bootloop). State: a smoke run passed
  (`C:\nexustest\tls\crops_smoke`); the full campaign was launched (`campaign_camp_script.txt`,
  `camp.out`) but its VM serial stops after boot — the run was cut off when the agent was
  stopped, so **the campaign has NOT been completed**. To finish: start `tlsserver.py`, run
  `campaign.py` (read its header), fix failures, then verify the real-site list:
  `tlsinfo example.com` (RSA/DigiCert), `www.google.com` (ECDSA/GTS), `www.cloudflare.com`,
  `letsencrypt.org` (ISRG), `curl https://example.com` contains "Example Domain",
  `curl https://www.wikipedia.org` (chunked), `wget https://example.com /tmp/ex.html`; each must
  print trusted=true with the matching root; nettests.py must still pass 57/57.
- Size: this stream alone adds ~86 KB; fine under the new 1.4 MB cap.

### 4.2 ws_browser — graphical browser (`handoff/ws_browser`)
- Files: NEW `kernel/webengine.c/h` (tokenizer → element tree → block/inline layout, images,
  links, tables, lists, entities), rewritten `kernel/browser.c/h` (window chrome: back/forward/
  reload/home, address bar, progress/status, scrolling, history, content-type dispatch:
  html/plain/image/download, redirects, untrusted-cert interstitial), NEW
  `kernel/netstream_compat.c` (**temporary** implementation of netstream.h over the old blocking
  HTTP; **DELETE it when merging together with ws_tls**, whose `netstream.c` is the real one),
  MODIFIED `desktop.c` (`browser_poll()` in the main loop + action hooks), `macui.c` (APPS row),
  `spotlight.c`, `shell.c` (`browse <url>`), `image.c/h`, `png.c`, `jpeg.c`, `gif.c` (decode-to-
  ARGB fixes/scaling), `html.c/h`, `build.bat`, `Makefile`. Worktree build: 924,044 B, 0 warnings.
- Tests present: `gen_site.py` (test site: index with headings/lists/table/links/images,
  long.html, 300 KB page, broken html, redirect, plain.txt), `bharness.py` (drives Spotlight →
  browser, Ctrl+L, typing, scrolling, link clicks, screenshots), `cropwin.py`, `pcapsum.py`.
  Five runs exist in `C:\nexustest\browser` (t1..t5, screenshots in `shots_t*`, host/http logs,
  pcaps). The latest run (t5) completed the whole script (long, bigload, big, broken, redirect,
  download, imgalone, chunked, reload) with `serial: ok`. The agent was investigating why run 2
  showed no image requests and only two index fetches ("incomplete transfer" check suspected);
  **look at `shots_t5` first** — if images render and links/back/reload work there, the bug was
  already fixed in later runs; otherwise debug `browser.c`'s load state machine against
  `http_t5.log`.
- Still to do: verify every screenshot by eye (layout, images at right size, table, links,
  scrollbar, title bar), real `http://example.com` / `http://info.cern.ch`, three apps open
  together, bootloop 5/5, no `!KR:`; then merge (after ws_tls) and retest https pages.

### 4.3 ws_apps — userland, libc, real apps (`handoff/ws_apps`)  — EARLY
- Written so far: `kernel/usys_abi.h` (syscall numbers/structs for the extended ABI),
  `kernel/usys.c/h` (syscall implementations: open/read/write/lseek/stat/getdents/spawn/
  waitpid/ioctl/... — review for completeness), `kernel/vt.c/h` (VT100/ANSI subset for the
  console + GUI terminal), `kernel/appstore_ui.c/h`, edits to `syscall.c/h`, `uproc.c/h`
  (FPU fxsave/fxrstor on switch, fd tables, spawn), `vga.c/h`, `keyboard.c/h` (raw-mode key
  escape sequences), `fat32.c/h`, `desktop.c` (terminal foreground jobs), `macui.c`, `shell.c`
  (bare-name → `/disk/bin` lookup), `build.bat`, `Makefile`; `userland/fatput.py` (FAT32 file
  injector for disk.img — verified to leave LBA ≥ 20480 untouched? the agent was verifying this
  when stopped), `userland/libc/user.ld`, `userland/libc/src/crt0.S`. Worktree build: 907,660 B.
  Downloaded sources: `C:\nexustest\apps\src\lua` (Lua 5.4.7) and `...\kilo` (untrusted
  third-party sources: compile only, never execute scripts from them).
- NOT done: the libc itself (stdio/stdlib/string/math via x87/time/setjmp/dirent/termios
  headers + libc.a build), `build.bat apps` target + fatput integration, the ports (Lua, kilo,
  2048, tetris, a tiny `sh` + coreutils), App Store/Launchpad entries, all tests. Follow the
  brief in `handoff/notes/phase57-in-progress.md` (agent 4) — it lists the required syscalls,
  libc coverage, apps, and the mandatory tests (Lua `print(2^10, math.sin(1))`, kilo edit/save,
  games, pipes in `sh`, FPU correctness with two concurrent float processes, GUI terminal running
  `lua` while the desktop stays responsive, persist unaffected by disk.img injection).
- Design constraints: user programs are static i386 ELFs loaded by `uproc.c` into their own
  address space (check `elf.c`/`uproc.c` for the expected load address — the legacy
  single-address-space loader window moved to 0xC80000 in the mem merge; the isolated path at
  0x08048000+ is the one to target); the kernel does not use the FPU; keep kernel-side
  additions small; apps live on `disk.img` (`/disk/bin`), not in the kernel.

### 4.4 Merge order and the two size limits
1. ws_tls → resolve → build → run `nettests.py` (57) + its campaign.
2. ws_browser → delete `kernel/netstream_compat.c` (and its build.bat/Makefile lines) → build →
   browser tests again now over real https.
3. ws_apps → build → its tests.
4. Final: `bootloop.py 10`, `fullcheck.py` all batches, `usbmatrix.py`, persist reboot test,
   `crashtest` set, GUI screenshot set; README "What's new"/"Known limitations" update + new
   screenshots (`screenshots/phase55-*.png` show the format).
Limits: kernel.bin ≤ 1,441,792 (build.bat guard); **`.bss` must end below 0xC60000** (linker.ld
ASSERT, ~1.1 MB free before this merge — the TLS stream's static buffers (TCP 16 KB x 8 rx,
TLS record buffers x 4) and the browser's static page/image pools will eat it: if the link
fails on the assert, shrink pools or raise `USER_LOAD_BASE` in `kernel/elf.h` (the legacy
loader window) upward and move the shmem/mmap constants accordingly).

## 5. Landmines (read before touching these areas)

- PIT runs at 18.2 Hz and the whole codebase assumes `system_ticks/18 = seconds` — never
  reprogram it. The kernel/desktop thread is never preempted; only ring-3 processes are.
  Never do heavy or blocking work in IRQ handlers; drivers are polled from the idle pumps
  (`shell_idle_pump()` in shell.c, the desktop loop in desktop.c, the login loop).
- Identity map covers 0..32 MB only; MMIO above that must be mapped with `paging_map_mmio()`
  before `uproc_init()` (per-process page directories copy the kernel PDEs at creation).
- kguard: faults in commands/app callbacks/desktop frames are recovered (`!KR:`), faults in
  IRQ handlers or during boot are fatal. The kernel image is read-only (W^X); page 0 is
  unmapped; a 16 KB guard band sits at 0x300000 under the boot stack. A #DF goes through a task
  gate whose TSS must carry the current CR3.
- Persistence: ramfs nodes created through the ramfs API are tracked (built-in vs user,
  tombstones); snapshots live at LBA 26624..32767 of `disk.img`; NPFS at 20480..24802; FAT32
  below 20480. Any tool that rewrites `disk.img` must preserve bytes ≥ LBA 20480.
- TCP servers must re-listen when the client closes (see rshell/sync/httpd/vnc poll patterns);
  `httpd_poll()` was missing from the idle pump until Phase 55. QEMU slirp: host = 10.0.2.2,
  DNS = 10.0.2.3; `HOSTFWD` in the harness exposes guest ports to 127.0.0.1.
- Display: QEMU GTK backend on Windows is the "2 fps" culprit; use `-display sdl` (build.bat
  does). A full 1080p `compose()` costs ~150-200 ms under TCG; dock/menu-bar hover now redraws
  only a band (`dock_under` buffer in desktop.c).
- USB: everything is polled; a hot-plug enumeration stalls the UI ~150-400 ms; low-speed
  devices cannot be emulated by QEMU (paths unverified).
- The heap is now 4 MB at 0x400000; PMM free pool after boot ≈ 4.9 MB; per-process kernel
  stacks are kmalloc'd 4 KB; the user frame pool is above 32 MB (`upmm_*`).

## 6. Open decisions for the owner
- Commit strategy for the ~180 changed/new files (nothing committed since `70871d6`).
- 64-bit (x86-64) port: deferred on purpose — it does not make Chrome/Windows/macOS binaries run
  (that needs their OS ABIs), and it would freeze all other progress; revisit after the userland
  exists (needs an x86_64-elf toolchain, long-mode boot, 4-level paging, and a sweep of ~300
  files for 32-bit pointer casts).
- SQLite / MicroPython ports (stretch goals in the apps brief) once the libc exists.
