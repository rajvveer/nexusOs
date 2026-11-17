# NexusOS handoff - 2026-10-09, Phase 57 integrated

This supersedes the previous handoff, preserved at
`handoff/notes/HANDOFF_BEFORE_DAILYUSE.md`. The ws_* trees are historical snapshots;
**do not apply their merge recipe again**.

## Current state

- Git root: `C:\Users\admin\Desktop\New folder`; project: `NexusOS/`.
- Branch `perf-1080p-modern-ui`, HEAD `70871d6`. No staging or commit performed.
- Last status: 141 tracked changes (including deletions), 78 untracked entries.
  Entries include whole directories, not just individual files.
- TLS, browser and userland/apps streams are integrated into the main tree.
- `build.bat apps` builds the OS and installs 25 program filenames in `/disk/bin`
  (including multicall aliases and self-checks). `build.bat run` launches it.
- Kernel: **1,042,860 bytes**, zero compiler warnings, cap **1,441,792**.
- Kernel SHA-256: `ceb2f9131d096f8695f4a7c1995600db65c9dfba1dab76c7a3cdf27a380a18e9`.
- Outputs: `nexus.img`, `nexus.iso`, `disk.img` with installed apps.
- Pre-integration backup: `C:\nexustest\before_dailyuse_20261009_220038`
  (source and images, excluding tools/handoff).

## What changed

- Reliable TCP, streaming HTTP/HTTPS, `curl`, `wget`, `tlsinfo`, `cryptotest`.
- TLS 1.3 AES-128-GCM/X25519, RSA/ECDSA authentication and built-in roots.
  Self-signed/unknown certificates now fail by default. Explicit `-k` permits
  diagnostic access; browser overrides apply only to the accepted origin.
  Certificate usage, critical constraints, validity and hostname are checked.
  Secure randomness uses RDRAND; there is no clock-based fallback.
- Graphical browser: basic HTML/images, links, history, scroll, redirects,
  downloads, certificate errors and verified-HTTPS indication. The temporary
  `netstream_compat.c` and obsolete `html.c/h` were removed.
- Picolibc 1.8.12 supplies actual stdio/stdlib/string/math/setjmp. The thin
  `userland/libc/src/platform.c` port connects it to NexusOS syscalls.
- Lua 5.4.7 and Kilo compile from unchanged upstream C sources. A small `sh`,
  file utilities, 2048 and Tetris run as isolated static i386 user processes.
- Process support includes fds, pipes, spawn/wait, close-on-exec, private pages,
  ANSI terminals and FPU save/restore. Background programs do not starve the
  parked desktop thread. Ring-0 remains cooperative.
- App Store, Spotlight and GUI Terminal launch real applications.
- Download failures preserve existing files; oversized/partial downloads are
  reported as failures. FAT installation uses a temporary image and atomic replace.
- Fixed a 16-bit TCP receive-capacity truncation that stalled large GUI reads at 64 KiB. A new known-answer test covers the boundary; streaming consumers no longer narrow the capacity.
- Fixed quoted shell operators, user-address upper-bound validation and initial
  FPU state isolation. Kernel and userland builds are warning-free.

## Build and use

From PowerShell in `NexusOS/`:

```powershell
.\build.bat apps
.\build.bat run
```

Login `root`/`root` or `guest`/`guest`; Ctrl+Space opens Spotlight, F12 opens the
kernel console. `apps` lists programs. Run `sh` for userland pipes/redirection.
Kilo saves with Ctrl+S and exits with Ctrl+Q. See `userland/README.md` for examples,
build setup and repeatable tests.

First app build needs Python 3.12+ and internet for checksum-pinned Picolibc and
pinned Meson/Ninja in `tools/userland-build`. Later builds use local caches.
The existing vendored i686-elf GCC compiles both kernel and userland. QEMU lives
at `C:\Program Files\qemu\qemu-system-i386.exe`.

Use **-cpu max -display sdl** for interactive QEMU. RDRAND is required for HTTPS.
GTK on Windows repaints slowly. Keep RTC in UTC for certificate checks.
Every kernel C file needs an explicit object in build.bat; crypto/layout/syscall
hot paths use -O2. The kernel has no libgcc; user programs may link libgcc.
`build.bat` must retain CRLF endings (LF produces spurious cmd.exe M commands).

## Verification

All VM tests use image copies under `C:\nexustest`.

| Check | Result / artifacts |
|---|---|
| TLS/HTTP campaign | 33/33 checks, `dailyuse_tls/serial_campaign.log` |
| Crypto/parser/TCP checks | 44/44 latest, `dailyuse_browser_fixed/serial_fixed.log` (43/43 in the earlier campaign) |
| Real HTTPS | example.com, Google, Cloudflare, letsencrypt.org trusted; Wikipedia fetched |
| Bulk transfer | 2 MiB HTTP and HTTPS with exact CRC, 20 repeat downloads, no leaked TCBs/streams |
| Existing servers | 58/58 host checks, `dailyuse_netfinal/host_reg.log` |
| Userland | Exact outputs checked for runtime/FPU, Lua, pipeline, game logic, Kilo save |
| GUI | Browser + CPU-busy Lua + Calculator, playable GUI-terminal games, `dailyuse_gui/` |
| Boot loop | 10/10 reached login, no faults, `dailyuse_bootfinal/` |
| ISO | Login/desktop reached, `dailyuse_iso/` |
| USB | UHCI, xHCI hub, EHCI companion hub, OHCI hub, `dailyuse_usb/` |
| Persistence | File contents survived hard reset, `dailyuse_persist/` |
| Crash containment | Null, UD, divide, write-protect, stack faults recovered, `dailyuse_kguard/` |
| Missing entropy | qemu32 rejects HTTPS even with -k; `dailyuse_no_rng/` |
| App Store | Installed apps and Open actions displayed; `dailyuse_store/` |
| FAT installer | Contents/tail, disk-full atomicity, corrupt-volume refusal passed |
| Final command campaign | 66 commands across 8 batches, no faults; `dailyuse_final_campaign/` |
| Extended browser pages | Long/malformed HTML, redirects, images, chunking/reload, escaped errors, 6144-byte download CRC FA0104A6; `dailyuse_browser_final/` |
| Interrupted download | Explicit error instead of saving a partial file; real HTTPS and 2 MiB TLS rechecked, `dailyuse_final_edges/` |
| Large-page regression | 300 KiB page reaches explicit 256 KiB cap; no 64 KiB stall, `dailyuse_browser_fixed/` |

```powershell
python -I userland/tests/test_fatput.py
python -I userland/tests/test_vm.py
python -I handoff/tools/bootloop2.py . C:\nexustest\boot_new 10
python -u -I handoff/tools/fullcheck.py . C:\nexustest\commands_new virtio floppy 56200
```

For TLS: launch `handoff/ws_tls/tlsserver.py <server-workdir>` on loopback
18101-18104, then `python -I handoff/ws_tls/campaign.py <workdir> <port> <tag> .`.
`tlscheck.py <serial.log>` returns nonzero on failure. Its diagnostic parser was
fixed to retain CN= text. Self-signed positive cases use -k; default rejection
is tested separately. `nettests.py` now checks oversized PUT at 262145 bytes.
`harness_net.py` uses the accompanying `pydes.py`.

Verified screenshots: `screenshots/phase57-*.png`. Normal tests must contain no
`!EX:` (fatal) or `!KR:` (contained bug). Only explicit crashtest injection should
produce !KR. Tests that merely print serial:ok are not sufficient to prove a
feature works: inspect output/screenshots or actual saved files.

## Memory/disk landmines

- PIT stays at 18.2 Hz. Never do blocking/crypto work in IRQ handlers.
- Heap: 4 MiB at 0x400000..0x800000.
- Normal .bss: 0x800000..0xC06B70, guard remains 0xC60000.
- Browser .app_bss: 0x02000000..0x024721E0, zeroed/reserved by pmm_init.
- Identity map: **40 MiB**, 10 page tables. High user frames begin at
  USER_PMM_BASE=0x02800000; keep it equal to IDENTITY_MAP_END. Linker asserts
  .app_bss ends below it. Legacy image/stacks/shmem/mmap and graphics addresses
  remain unchanged. Map MMIO before creating user process page directories.
- 16 MiB disk: FAT32 below LBA 20480; NPFS 20480..24802; RAM persistence
  26624..32767 (two 1.5 MiB A/B slots).
- fatput validates the protected tail, detects concurrent image changes, then
  replaces the disk atomically. Only an entirely empty FAT window is formatted.
  The installed main disk's bytes from LBA 20480 onward match the backup exactly.
- Test work directories must have no spaces (QEMU screendump/drive path gotcha).
- User apps use private virtual memory, entry 0x08048000, 256 KiB stack and
  per-process heap. They are stored on FAT, not linked into kernel.bin.

## Boundaries and next work

This remains an independent development OS, not universal desktop compatibility.
Existing Chrome/Office, ordinary Windows/macOS applications and Linux desktop
binaries do not run. C programs must target NexusOS; compilation currently runs
on the host. x86-64 remains deferred and would not by itself provide those ABIs.

Browser: no JavaScript engine, modern CSS or full Unicode; one page at a time,
256 KiB page/image-download cap and bounded image pools. Downloads go to RAM-FS
(256 KiB/file), not streaming to FAT. DNS/handshake work can briefly stall the
cooperative UI. Existing download files are not overwritten silently.

TLS: no TLS 1.2, HelloRetryRequest, resumption, client certs, OCSP/CRL or independent
crypto audit. Unsupported certificate constraints fail closed. Physical hardware
and actual audio/video playback were not newly validated.

Userland: no fork/exec replacement, job control, sockets API, full POSIX signals
or complete mmap/munmap (unsupported munmap fails explicitly). FAT remains 10 MiB;
RAM snapshots can omit files when their 1.5 MiB slot fills, with a status warning.
SQLite/MicroPython and richer browser/application compatibility remain follow-up
work. Do not label the whole OS production-ready. No commit is authorized yet.
