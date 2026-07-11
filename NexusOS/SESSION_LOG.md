# NexusOS — Development Session Log

A running log of work done in an assisted development session. Most recent
session at the top. Dates are approximate (kernel `currentDate` was 2026-06-04).

---

## Session — Phase 51: NPFS Journaling Filesystem  *(NOT committed yet, 2026-06-16)* — ROADMAP COMPLETE 🎉

Implemented **Phase 51 — NPFS (NexusOS Persistent File System)**, the FINAL
phase: a from-scratch, crash-consistent **journaling** filesystem. This
completes the 51-phase roadmap (**51/51, 100%**). One new module
(`npfs.c/h`) + shell/kernel/build wiring. Shell **v36.0 / 142 commands**.
All deliverables verified live in QEMU including a power-cycle persistence test;
**3 real bugs found and fixed during verification.**

### Headline: a real write-ahead journal with crash recovery
- Every mutation is a transaction: write a **journal descriptor** + the new
  block images + a **COMMIT record** (carrying a CRC32 over the whole txn) to
  the on-disk journal region BEFORE touching the blocks' real homes, then
  **checkpoint** (copy logged blocks home) and clear the commit record.
- `npfs_mount` **replays** any committed-but-uncheckpointed transaction (verifies
  the CRC first; a partial/uncommitted txn is ignored, leaving the FS unchanged).
- Proven by **`npfs crashtest`**: writes a file with the checkpoint deliberately
  SKIPPED (simulated crash after commit) → pre-replay read returns 0 bytes →
  remount triggers replay (`[journal] replayed txn N`) → post-replay read
  returns the file intact → **RESULT: PASS** (verified on screen).

### On-disk layout (in a reserved high-LBA window, LBA 20480+ of the IDE disk)
- `[superblock][journal 192 blk][inode table 32 blk][block bitmap 1 blk][data]`.
- **Superblock**: magic/version/geometry/next-txid/**mount-count** + CRC32.
- **Inodes**: 128 fixed, exactly 128 B each (`_Static_assert`), with 12 direct
  block pointers + 1 single-indirect block (128 ptrs) → ext2-style large-file
  structure (not a single chain).
- Never auto-formats: `npfs_mount` only succeeds on a valid superblock, so the
  **FAT32 region is untouched** unless the user runs `npfs format`. The Phase-50
  installer's MBR (LBA 0/2048) is far below the NPFS window — no collision.

### Verified live (QEMU headless)
1. `npfs format` → journaling FS ready.
2. `npfs write a.txt hello world` → journaled; `npfs cat a.txt` → "hello world".
3. `npfs stat` → correct geometry + **Data blocks 2/4096 used** (after the
   bitmap fix), journal 192 blocks, inodes 2/128.
4. `npfs crashtest` → **PASS** (journal recovered a file after a simulated crash).
5. **Persistence across reboot**: rebooted the SAME disk.img with NO reformat →
   NPFS **auto-mounted at boot**, both files survived with exact content, and the
   **mount counter advanced 0→1** (proves the superblock persisted to disk).
   Host-side inspection of disk.img confirmed magic `0x4E504653` + a correct
   bitmap.

### Bugs found & fixed during verification (this is why we test live)
- **Bitmap stack over-read**: the bitmap is ONE 512 B block (4096 bits) but the
  stats/alloc loops iterated over all ~12190 data blocks, reading past the
  buffer (the bogus "268 blocks used"). Fix: cap `data_blocks` to 4096 (one
  bitmap block) at format time, so every consumer stays in bounds.
- **Multi-alloc-in-one-txn collision**: a multi-block file calls `bitmap_alloc`
  several times in one transaction; each re-read the (uncheckpointed) on-disk
  bitmap and picked the SAME free bit. Fix: `txn_add` now **replaces by LBA**
  (no duplicate staged blocks), and `bitmap_alloc`/`inode_stage` read the
  **staged** copy via `txn_find` so successive allocations compose.
- **64 KB transaction on the kernel stack**: the per-op `npfs_txn_t` (125 block
  images) is far too big for the stack. Fix: two file-scope static txn buffers
  (commit + replay) — safe in the single-threaded cooperative kernel.

### Shell / wiring
- 1 new command `npfs` (12 subcommands: format/mount/ls/new/write/cat/rm/stat/
  journal/crashtest/help) → **v36.0 / 142 commands**. Help banner v35→v36,
  welcome v35→v36, new "NPFS:" help row, header notes "ROADMAP COMPLETE (51/51)".
- `kernel.c`: `npfs_init()` after `finale_init()` (auto-mounts if present;
  needs `ata_init`, done earlier). `build.bat`: linked `npfs.o`.
- Reuses `pkg_crc32` for journal/superblock checksums (no duplicate CRC, no
  64-bit math). Integer-only; `npfs.c` compiles with **zero warnings**.

### Deferred (honestly flagged)
- Flat root namespace (no subdirectories yet — inodes/dir-entries support it but
  the dir tree isn't built).
- Single-block bitmap caps a volume at 4096 data blocks (2 MiB); a multi-block
  bitmap would lift it. Max single file = 122 blocks (61 KB), bounded by what
  one journal transaction can hold (descriptor is one 512 B block = 125 LBAs).
- No VFS integration (NPFS is its own `npfs` command namespace, parallel to the
  RAM-FS that backs `ls`/`cat`); mounting it into the unified VFS is future work.
- Not wired as the boot/root FS — it's a persistent data volume alongside FAT32.

---

## Session — Phase 50: v5.0 Grand Finale  *(NOT committed yet, 2026-06-16)*

Implemented **Phase 50** (Era 7 FINALE — "World Domination"), the v5.0 capstone.
One new module (`finale.c/h`) + shell/kernel/build wiring + a build-side ISO
generator (`mkiso.py`). Shell **v35.0 / 141 commands**. Floppy image ~707 KB
(`Image size: 707184`); **kernel image itself measured 688 KB on the report
card** (image bytes vs. floppy padding differ). All four pillars verified live
in QEMU, plus host-side disk verification.

### Pillar 1 — Universal binary launcher (`urun <file>`)
- `finale_detect_format()` sniffs magic bytes: `0x7F 'ELF'` → ELF32, `MZ` → PE32,
  `0xFEEDFACE/CF` (either endian) → Mach-O. `finale_urun()` reads the file from
  the VFS, validates with the matching `*_validate`, and dispatches to the
  existing loader (`elf_exec` / `pe_exec` / `macho_exec`, Phases 20/34/37). One
  command for any supported format; unknown formats are rejected cleanly. (No
  PE/ELF sample is bundled in the RAM-FS, so live `urun` exercises the
  detect+dispatch path; the underlying loaders were verified in their own
  phases via `runexe`/`runmacho`.)

### Pillar 2 — Footprint report card (real numbers)
- `finale_kernel_size()` = `_kernel_end - _kernel_start` (linker symbols);
  `finale_ram_used()` = `pmm_get_used_pages() * 4096`. `finale_show_stats()`
  asserts the v5.0 goals: **Kernel image 688 KB / limit 1024 KB → [PASS]**,
  **RAM in use 1736 KB / limit 16384 KB → [PASS]** (measured live). Both
  roadmap targets ("< 1 MB kernel, < 16 MB RAM") met and shown with the
  full page breakdown.

### Pillar 3 — Disk installer (`install` / `install disk yes`)
- Writes a **real MBR** to the primary-master IDE disk via `ata_write_sectors`:
  a tiny halt boot stub + a partition table at 0x1BE (entry 1 = active, type
  0x0C FAT32-LBA, start LBA 2048, full remaining size) + the 0xAA55 signature,
  then a system manifest at the partition's first sector. **Then verifies by
  reading both back** (signature, partition entry, manifest bytes).
- Two-step safety: bare `install` (or `install disk`) prints the layout as a
  **DRY RUN**; only `install disk yes` writes. Contained to the QEMU IDE image
  — `ata.c` targets the primary master exclusively; the boot floppy is a
  separate controller, so the host is never touched.
- **Host-side proof**: after the guest run, inspecting `disk.img` directly
  showed MBR sig `0xAA55`, part-1 boot flag `0x80`, type `0x0C`, **start LBA
  2048, 30720 sectors**, and the manifest "NEXUSOS-INSTALL v5.0" at LBA 2048.

### Pillar 4 — Bootable ISO generation (build-side)
- `build.bat` now produces `nexus.iso` after padding the floppy image: it tries
  `xorriso`/`mkisofs` if present, else falls back to a dependency-free
  **`mkiso.py`** — a minimal ISO9660 writer (PVD + El Torito Boot Record +
  boot catalog + root dir) using **floppy emulation (media 0x02)** pointing at
  `nexus.img`. A BIOS booting the ISO emulates it as A: and runs the exact same
  Stage-1 loader, so no separate boot path is needed.
- **Verified**: `nexus.iso` (1.5 MB) booted in QEMU via `-cdrom nexus.iso -boot
  d` straight to the NexusOS login screen.

### Shell / wiring
- 3 commands (`urun`, `install`, `finale` + `v5` alias) → **v35.0 / 141
  commands**. Help banner v34→v35, welcome v34→v35, new "Finale:" help row.
- `kernel.c`: `finale_init()` after `appstore_init()` (needs ata/pmm/loaders,
  all up earlier). `build.bat`: linked `finale.o` + the ISO step.

### Screens
- `screenshots/finale-report-card.png` (banner + footprint PASS/PASS),
  `finale-installer.png` (dry-run plan + real verified write),
  `finale-iso-boot.png` (NexusOS booted from the ISO).

### Deferred (honestly flagged)
- "Universal binary support" runs binaries only to the extent the PE/ELF/Mach-O
  **compat shims** implement (subset APIs) — not full Win/Mac/Linux coverage.
- The installer stages an MBR + manifest, not a full FAT32 filesystem copy of
  every system file onto the partition (the floppy remains the boot medium; a
  full self-hosting install needs a FAT32 *writer* + relocating the boot chain
  to the partition — a larger lift).
- `mkiso.py` is a minimal ISO9660 writer (no Joliet/Rock Ridge, single boot
  file); fine for a bootable disc, not a general-purpose mastering tool.
- Phase 51 (NPFS journaling filesystem) is the only remaining phase.

---

## Session — Phase 49: App Store & Ecosystem  *(NOT committed yet, 2026-06-16)*

Implemented **Phase 49** (Era 7 "World Domination" OPENER), scoped honestly: a
curated **storefront layered over the Phase-35 package manager** (`pkg.c`)
rather than a re-implementation. One new module (`appstore.c/h`) + shell/kernel/
build wiring. Kernel 687 KB → ~700 KB (`Image size: 700336 bytes`, padded to
1.44 MB; ~20 KB headroom under the ~720 KB ceiling — getting tight). All four
deliverables verified live in QEMU.

### Design principle: delegate, don't duplicate
- The App Store does **not** re-implement install/remove/dependency-resolution/
  CRC integrity. `appstore_install` → `pkg_install`, `appstore_upgrade_all` →
  `pkg_remove`+`pkg_install`, update checks → `pkg_update`/`pkg_repo_revision`/
  `pkg_version_cmp`. So the already-reviewed pkg engine stays the single source
  of truth; the storefront only adds the **ecosystem layer** on top.
- The catalog (`app_listing_t catalog[]`) **decorates** repo packages by name
  (category / 1–5★ rating / featured / trust tier / tagline). Versions, deps,
  and descriptions are read live from `pkg_repo_find` — never copied — so the
  two stay loosely coupled. A listing whose package isn't in the repo is
  silently skipped by the views.

### Four pillars
1. **Catalog / storefront** — `store` (featured rail), `store list` (all 8 apps
   grouped by System/Productivity/Games/Other), `store category <c>`,
   `store search <term>` (name/description/tagline), `store info <app>` (detail
   page: rating, author, deps, sandbox tier, install status + "update
   available"). `[installed]` markers reflect the live pkg DB.
2. **Automatic updates** — `store update` re-probes the mirror (`pkg_update`,
   bumps the repo revision) and compares each installed package's version to
   the repo version via `pkg_version_cmp`, listing outdated ones. `store
   upgrade` snapshots the outdated names first (so the install-DB indices it
   iterates don't shift mid-loop), then remove+reinstall through the pkg engine
   (which re-resolves deps). `appstore_outdated_count()` is the headless probe.
3. **Developer SDK** — `store sdk` prints the API quick-reference; `store sdk
   install` scaffolds four files into the RAM-FS (`sdk-readme.txt`,
   `sdk-api.txt`, `sdk-manifest.txt`, `sdk-sample.c`) — a buildable hello-world
   app template + a package manifest + the kernel API surface an app may call,
   readable with `cat`. Files reuse an existing node if present (idempotent).
4. **App sandboxing (HONEST)** — `store sandbox <app>` and the install path
   apply a **cooperative** trust profile (`APP_TRUST_CORE/TRUSTED/SANDBOXED`),
   which is uid/permission scoping (Phase 44), **NOT memory isolation**. The
   profile explicitly states: shared address space, ring 0, enforced only at
   the VFS read/write choke points; per-process page dirs + ring-3 (true
   isolation) is the deferred Phase-44 address-space rewrite. Install of a
   `SANDBOXED` app prints the caveat inline.

### Shell / wiring
- 1 new command `store` (10 subcommands) → shell **v34.0 / Phase 49 / 138
  commands**. Help banner v33→v34, welcome v33→v34, new "App Store:" help row.
- `kernel.c`: `appstore_init()` after `mobile_init()` (needs `pkg_init` +
  `users_init`, both already run earlier). `build.bat`: linked `appstore.o`
  (compilation auto-globs `kernel\*.c`; only the explicit link list needs the
  entry).

### Verified live (QEMU headless, monitor sendkey drive + screendump)
- Boot → login (root) → Esc to text shell, then: `store list` rendered all 8
  apps grouped by category with stars/versions/taglines and `[installed]` on
  `hello`; `store info doom` showed the detail page; `store install hello` ran
  through the pkg engine and applied the sandbox profile; `store update`
  reported "All apps are up to date"; `store sdk install` wrote 4/4 SDK files;
  `store sandbox doom` printed the honest cooperative profile ("shared, ring 0
  — NOT memory-isolated"). Screens captured to `screenshots/appstore-list.png`
  and `appstore-sdk-sandbox.png`.

### Deferred (honestly flagged)
- Real internet App Store (offline; reaches the same bundled mirror as `npkg`).
- True app sandboxing = the deferred address-space isolation (per-process page
  dirs + ring-3); today it's cooperative uid/perm scoping only.
- A GUI storefront window (today `store` is CLI-only).
- Persisting the installed set across reboot (the pkg DB is in-RAM, like the
  Phase-44 user DB / Phase-45 settings.cfg).
- The SDK template is a doc/scaffold, not an end-to-end `.npk` build toolchain
  (the codec exists in `pkg.h` `npk_serialize`; wiring a `store publish` that
  builds a user app into an .npk is a future step).

---

## Session — Phase 48: Performance Optimization  *(NOT committed yet, 2026-06-15)*

Implemented **Phase 48** (Era 6 FINALE), scoped honestly for a single-CPU QEMU
i386 target. One new module (`perf.c/h`) + an optimized `string.c` + a small,
boot-safety-first kernel.c change. Kernel 683 KB → 687 KB. Adversarial review
found **0 correctness bugs**. All deliverables verified live.

### Real, measurable optimization: rep-string memcpy/memset (`string.c`)
- Replaced the naive byte-loop `memcpy`/`memset` with `rep movsl`/`rep stosl`
  fast paths (4-byte bulk + byte tail) for aligned blocks — the hot caller is
  `fb_flip`'s ~3 MB framebuffer copy. **Overlap-safe**: the only overlapping
  caller (tcp.c rx-buffer left-shift, dst<src) takes the forward fast path
  correctly; a defensive backward-copy branch covers the dst>src case no caller
  hits today. Integer-only, `cld` explicit, `(uint32_t)` casts (no `uintptr_t`
  in types.h). Measured: **memcpy ~2917 MB/s, memset ~3198 MB/s** (vs a byte
  loop's ~hundreds of MB/s).

### Boot-time measurement + "boot < 1s" (kernel.c, perf.c)
- `perf_mark_boot_done()` stamps `system_ticks` at the end of automated init
  (after the last _init, before the interactive login). `perf`/the boot banner
  report it. Measured: **3 ticks ≈ 165 ms — "[under 1 second]"** (the roadmap
  goal, measured & met). Resolution is ±55 ms (PIT is the default 18.2 Hz — NOT
  reprogrammed; reprogramming would break the codebase-wide `ticks/18=seconds`
  assumption, so it was deliberately left alone).

### Boot reliability: the IRQ init guard (kernel.c) — also a perf/stability win
- Root cause (confirmed): `idt_load` does `sti` before `pic_init` unmasks IRQs,
  so the timer IRQ0 fires during ALL of init while pmm/heap/rtc/VGA/net are
  still being built; the IRQ handler's status-bar redraw (reads rtc/pmm/VGA) and
  `net_poll` running mid-init is the suspected trigger of the long-standing
  intermittent boot #DF. Fix: a `volatile bool boot_init_done` — `system_ticks++`
  stays unconditional, but the status-bar/`net_poll`/`schedule` work is gated
  behind the flag (set true at the end of init). The PIC EOI is already sent by
  `irq_handler` before the callback, so the early return is PIC-safe. No boot
  fault occurred across the many reboots this session.

### Honest SMP / preemption status (perf.c) — NOT faked
- `smp`: reports `CPUs online: 1` (QEMU single-core; NexusOS is uniprocessor)
  and exactly what real SMP needs (ACPI MADT, Local APIC/IO-APIC, AP bring-up
  via INIT-SIPI-SIPI, per-CPU state + spinlocks everywhere). `preempt`: the
  round-robin scheduler exists but is intentionally left disabled
  (`scheduler_start()` is never called) — the kernel is one shared address space
  with no locks, so preempting mid-syscall would corrupt the heap/VGA/VFS and
  worsen the boot fault. Decision (validated by a design panel): **do NOT enable
  preemption / SMP** — it would be strictly negative-EV here. Documented, not
  pretended.

### perf benchmark command
- `perf` runs the tick-boundary benchmark harness (same trick as `gpubench`):
  reports boot time, memcpy/memset MB/s, and an int-loop Mops/s baseline, then
  the SMP/preempt status. `smp`/`preempt` print just their sections.

### Wiring
- `kernel.c`: `boot_init_done` flag + timer_callback guard + `perf_mark_boot_done()`
  + boot-time print + include. `shell.c`: 3 commands (`perf`/`smp`/`preempt`) +
  a Perf help row + header **v33.0 / Phase 48 / 137 commands** + welcome v32->v33
  + include. `string.c`: optimized memcpy/memset (no header/caller changes — same
  signatures). `build.bat`: links `perf.o`. NO changes to scheduler.c / idt_asm /
  the memory layout (deliberately — those are the high-risk landmines).

### Verified live (headless QEMU, captured via VNC/monitor)
- Boot banner + `perf` show **Boot 3 ticks (~165 ms) [under 1 second]**,
  **memcpy 2917 MB/s, memset 3198 MB/s, int loop 200 Mops/s**. Desktop + VNC
  render perfectly = the rep-string asm is correct (fb_flip uses it). `smp` →
  CPUs online 1 + requirements; `preempt` → cooperative + reasoning. No `!EX:`
  boot fault all session (the guard works). Screenshot:
  `screenshots/perf-benchmark.png`.

### Deferred (honestly flagged, NOT done)
- True SMP and kernel preemption (huge, negative-EV rewrites — documented via
  `smp`/`preempt`). Reprogramming the PIT for finer timing (would break ~dozens
  of files' `ticks/18` assumption). The definitive boot-fault fix (move `sti`
  out of `idt_load`) — the guard is the minimal mitigation; the root-cause fix
  is a separate, larger change. Raising the kernel size ceiling / fixing the
  heap-stack-fb layout overlap (the boot-fault landmine) — left alone since the
  guard mitigates the symptom and Phase 48's additions are tiny.

---

## Session — Phase 47: Mobile/Embedded Mode  *(NOT committed yet, 2026-06-15)*

Implemented **Phase 47** (Era 6), scoped honestly for a QEMU i386 target (no
real touchscreen / ARM CPU here). One new module (`mobile.c/h`) + small wiring
into desktop/shell/vnc. Kernel 677 KB → 683 KB (~37 KB under the ~720 KB
ceiling). Integer-only (`nm mobile.o` clean). All four pillars verified live.

### Touch / gesture layer (pillar 1)
- `mobile_poll()` observes the shared PS/2 mouse state (`mouse_get_state`,
  read-only — existing desktop click handling untouched) and runs a gesture
  state machine over pointer-down -> move -> up: **tap** (short, still),
  **long-press** (held, still), **drag** (moved a little), and **swipe**
  up/down/left/right (moved past a threshold, classified by dominant axis using
  the same Manhattan metric as the travel tracker so diagonals count).
- Driven from three same-thread sites: `desktop_run` per frame, `shell_idle_pump`
  while the shell idles, AND `vnc.c inject_pointer()` after each injected VNC
  PointerEvent — the last is what makes a remote (VNC) press->drag->release get
  sampled at full event rate instead of once per idle loop (the fix that made
  the swipe test pass). `clicked` only fires on press not release, so the
  recognizer tracks button transitions itself.

### Responsive UI / orientation (pillar 2)
- A **logical** orientation model: `orientation portrait|landscape|toggle`.
  `mobile_screen_w/h()` report the orientation-adjusted size (swapped in
  portrait: 1024×768 -> 768×1024) for layout-aware code to consult. We do NOT
  physically rotate the framebuffer (every blitter assumes width=1024 — a
  separate large rewrite); this is honestly labeled a logical model, not a real
  display rotation.

### Low-power mode (pillar 3)
- `lowpower on|off` switches the desktop's idle (clock) redraw cadence from
  `REDRAW_NORMAL` (18 ticks, ~1s) to `REDRAW_LOWPOWER` (72 ticks, ~4s) via
  `mobile_redraw_interval()`, which the desktop loop now reads instead of a
  hardcoded 18 — so an idle desktop spends ~4× longer in HLT. Real input still
  forces an immediate redraw (the `if(inp)` path), so responsiveness to actual
  interaction is unaffected.

### ARM / embedded status (pillar 4)
- `arm` prints an HONEST report: current target `i686-elf (x86, 32-bit)`, ARM
  toolchain present: no, and exactly what a real AArch32/64 port needs (cross
  toolchain, ARM boot path, rewriting the x86 HAL — GDT/IDT/PIC/port-I/O/cli-hlt/
  context-switch/paging -> GIC/MMIO/WFI/MMU, an ARM framebuffer driver). No fake
  port is claimed.

### Wiring
- `kernel.c`: `mobile_init()` after `assistant_init()` + include. `desktop.c`:
  `mobile_poll()` per frame + `mobile_redraw_interval()` for the idle cadence +
  include. `shell.c`: 5 commands (`gesture`, `orientation`, `lowpower`, `arm`,
  `mobileinfo`) — note `touch` was already the file-create command, so the touch
  *mode* command is `gesture`; a **Mobile:** help row; `shell_idle_pump` calls
  `mobile_poll`; header bumped to **v32.0 / Phase 47 / 134 commands**; welcome
  v31->v32. `vnc.c`: `inject_pointer` samples `mobile_poll` + include.
  `build.bat`: links `mobile.o`.

### Adversarial review: 5 categories, 0 bugs
- The review specifically chased the re-entrancy risk (mobile_poll now called
  from 3 places) and refuted it: all same cooperative thread, mobile_poll is
  IRQ-free and atomic per call, statics are never touched concurrently — extra
  invocations just re-sample, which is the intended benefit. argv bounds, the
  int_to_str buffers, the redraw-cadence cast, and orientation reads all checked
  out. Applied one suggested feature improvement (diagonal swipes use the
  Manhattan metric so they classify as a swipe, not a drag) and re-verified.

### Verified live (headless QEMU, captured via the VNC client)
- `mobileinfo` / `arm` print correctly (target x86, ARM toolchain: no).
- `orientation portrait` -> logical 768×1024; `lowpower on` -> redraw every 72
  ticks. `gesture on` + injected VNC pointer sequences -> recognized
  **swipe-right**, then **tap** + **swipe-up** (count=2). No boot fault.
  Screenshots: `screenshots/mobile-gesture-swipe.png`,
  `mobile-orientation-power.png`, `mobile-info-arm.png`.

---

## Session — Phase 46: AI Assistant  *(NOT committed yet, 2026-06-15)*

Implemented **Phase 46** (Era 6): an **offline, rule-based** command assistant —
no internet LLM is reachable from a freestanding kernel, so this is an honest
heuristic helper, not a model. One new module (`assistant.c/h`) + a Tab
autocomplete hook in the editor. Kernel 657 KB → 677 KB (under the ~720 KB
ceiling); no new warnings; integer-only (`nm` shows no 64-bit math in
`assistant.o`). All four pillars verified live in QEMU.

### Natural language -> command (`ask "<text>"`)
- Tokenizes + lowercases the request (local `lc`/`is_alnum`/tokenizer — string.c
  has none), drops stopwords, and scores it against a ~55-entry **intent table**
  (keyword sets -> command templates). Scoring: exact keyword +10, stem (>=4
  chars) +6, the command name spoken literally +12, verb-at-front +4, >=2
  distinct keywords +5. Extracts an argument (filename / number / url / theme /
  "rest") from the *original* (case-preserved) string.
- **Confidence gates**: RUN needs best>=12 AND margin>=4; below that it explains
  instead of running. **Destructive guard**: `rm`/`kill`/`chmod`/`firewall`
  never auto-run unless best>=20, margin>=6, AND a real argument was extracted —
  otherwise it prints the command for the user to run. `run==false` is a hard
  explain-only override.
- Two real codebase bugs the design panel caught and the matcher now handles:
  `fontsize` does NOT clamp (rejects out-of-range), so the assistant clamps to
  1..4 before emitting; theme names are exactly `dark light retro ocean hicon`
  (whitelisted in arg extraction).

### CLI help / knowledge base (`ai [topic]`)
- An embedded `kb_entry_t` table (name/usage/desc/group) — the machine-readable
  command list the shell never had. `ai` = grouped overview, `ai <cmd>` =
  description + usage + related commands, `ai <free text>` = best-match.

### Smart file search (`find <query>`)
- Fuzzy filename match (case-folded substring + subsequence) AND **content
  grep**: reads each RAM-FS file via `vfs_read` into a static 4 KB buffer,
  substring-searches the bytes, and prints the matching line as a snippet.
  Goes beyond the names-only `search` GUI app.

### Editor autocomplete (Tab)
- `editor.c` Tab now completes the word-at-cursor via `assistant_complete()`
  against a built-in C / NexusScript keyword list **plus words already in the
  buffer**; repeated Tab cycles candidates; typing/Enter cancels the cycle.

### Wiring
- `kernel.c`: `assistant_init()` after `users_init()`. `shell.c`: 3 commands
  (`ask`, `ai`, `find`) with a `shell_join_args` free-text reassembler + an
  **AI:** help row; header bumped to **v31.0 / Phase 46 / 129 commands**;
  welcome banner v30->v31. `editor.c`: `#include assistant.h`, Tab case, footer
  hint. `build.bat`: links `assistant.o`.

### Bug found & fixed (adversarial review: many checks, 1 real bug)
- **Editor autocomplete `ac_inserted` desync on a full line.** The insert loop
  incremented its counter unconditionally, but `insert_char` is a no-op once the
  line hits `EDITOR_MAX_COLS-2` — so on a near-full line `ac_inserted`
  over-counted, and the next Tab's delete loop would eat into the user's own
  text and even join lines. Fixed to count only insertions that actually grew
  the line (and stop when full). Re-verified: `whi`+Tab -> `while` still works.
  (The review's other flagged concerns — tokenizer off-by-one, kw_score
  over-read, find buffer bound, short-candidate over-read — all traced safe.)

### Verified live (headless QEMU, captured via the Phase-45 VNC client)
- `ai` overview renders. `ask what time is it` -> `-> date` -> **ran**
  (printed 15/06/2026 10:29:33). `ask how much memory is free` -> `-> meminfo`
  -> **ran** (Free 31060 KB). `ask delete report` -> `-> rm report` ->
  **refused to run** (destructive guard). `find welcome` -> readme.txt by
  content; `find nexus` -> 4 files with snippets. Editor `ret`+Tab -> `return`,
  `whi`+Tab -> `while`. No boot fault. Screenshots:
  `screenshots/ai-ask-nl-command.png`, `ai-smart-find.png`,
  `ai-editor-autocomplete.png`, `ai-destructive-guard.png`.

---

## Session — Phase 45: Cloud & Sync  *(NOT committed yet, 2026-06-14)*

Implemented **Phase 45** (Era 6): all four "Cloud & Sync" pillars — a real VNC
remote-desktop server, clipboard sync, a cloud file-sync protocol, and settings
sync. Two new modules (`vnc.c/h`, `sync.c/h`) plus small input-injection /
idle-pump hooks across keyboard/mouse and per-frame polls in desktop/shell.
Kernel 638 KB → 657 KB (under the ~720 KB ceiling); no new warnings. **All four
verified live against real clients** (a from-scratch Python RFB client + a sync
protocol client over QEMU hostfwd). No boot fault during the session.

### VNC / RFB remote desktop server (`vnc.c/h`, port 5900)
- From-scratch **RFB 3.3** server mirroring the rshell/httpd listener pattern
  (one `tcp_listen` TCB slot, polled, re-listens after a client drops). Speaks
  ProtocolVersion → security type 1 (None) → ClientInit → ServerInit.
- **PIXEL_FORMAT 32bpp/depth24/little-endian/true-colour, shifts R16 G8 B0** —
  byte-identical to the `fb_get_backbuffer()` 0x00RRGGBB back buffer, so pixels
  go on the wire with **zero conversion**.
- **Raw encoding only**, sent as **full-width dirty row-bands** (x=0, w=1024) so
  each band is a contiguous zero-copy slice of the back buffer. 32-bit FNV-1a
  **tile hashing** (192 tiles, sub-sampled 4×4, hashed in a rolling window of 24
  tiles/poll) drives dirty detection; updates are pull-model (one per
  `FramebufferUpdateRequest`) and banded (≤8 rows / 32 KB per `tcp_send`, ≤64
  rows/poll). **Integer math only** — verified `nm` shows no `__udivdi3` etc.
- **Input injection** via new hooks: RFB `KeyEvent` → `keyboard_inject_char`
  (keysym→ASCII incl. arrows→0x80–0x83), RFB `PointerEvent` → `mouse_inject`
  (RFB L/M/R bits remapped to mouse.h L/R/M). Partial-message guards + a
  `skip_bytes` counter discard oversized `SetEncodings`/`ClientCutText` without
  growing the 300-byte parse buffer.
- Verified: a Python RFB 3.3 client (`vnc_capture.py`) connected over hostfwd
  5900, completed the handshake (`ServerInit 1024x768 name='NexusOS'`), and
  **captured a pixel-perfect full 1024×768 frame** showing the live shell —
  twice in a row (proves the re-listen). Colors/text correct ⇒ pixel format
  right.

### Clipboard sync (over the VNC channel)
- RFB `ClientCutText` → `clipboard_copy` (in) and a polled diff of
  `clipboard_paste()` → `ServerCutText` (out), gated so it isn't echoed back.
- Standalone `clipsync [set <text>]` exposes the local clipboard on the CLI.
- Verified: sent `ClientCutText "hello-from-vnc"` over RFB → guest `clipsync`
  printed **`Clipboard: hello-from-vnc`**.

### Cloud file sync (`sync.c/h`, port 7070)
- A tiny line-oriented TCP protocol: `LIST` / `GET <name>` / `PUT <name> <size>`
  + body / `BYE`; server replies `FILE <name> <size>`+bytes / `END` / `OK`/`ERR`.
  Server mirrors the rshell pattern with **PUT body reassembly across the 1 KB
  RX window**; client ops are blocking + tick-bounded (pump `net_poll`+`hlt`).
  Respects the 4 KB RAM-FS per-file cap and Phase-44 VFS permission checks.
- Shell: `sync serve|stop|list <ip>|pull <ip> <f>|push <ip> <f>|pullall <ip>|status`.
- Verified: host client (`sync_client.py`) over hostfwd 7070 → `LIST` showed all
  9 RAM-FS files w/ sizes, `GET readme.txt` returned the exact 157-byte body,
  `PUT hostfile.txt` returned `OK`, and a **round-trip `GET hostfile.txt`** read
  the 48 bytes back ⇒ full bidirectional file sync.

### Settings sync
- `settings_serialize` reads the live config (theme/font-scale/reader/contrast/
  wallpaper) into a `# NexusOS settings v1` key=value blob; `settings_save`
  writes it to `settings.cfg` in the RAM-FS (so it rides the file-sync channel),
  `settings_load` parses + re-applies it (theme_set / font_set_scale /
  accessibility / wallpaper_set).
- Shell: `synccfg [save|load] [path]` (no arg = show current).
- Verified round-trip: `theme ocean` → `synccfg save` (74 B) → `theme dark` →
  `synccfg load` → `synccfg` showed **`theme=3`** (reverted to ocean) ⇒
  serialize→file→deserialize→apply works.

### Wiring + servicing model
- The servers are polled from non-IRQ context: per-frame in `desktop_run`
  (`net_poll`+`vnc_poll`+`sync_poll`), and — new this phase — from a
  **keyboard idle hook** so a backgrounded server keeps working at the text
  shell too. `keyboard_getchar()` blocks on `hlt`; it now calls an installable
  `keyboard_set_idle_hook()` each wait iteration, and `shell_run` installs
  `shell_idle_pump()` (= net/vnc/sync poll). The `vnc serve` command is also an
  interactive blocking serve loop (Esc to return; server keeps running).
- `kernel.c`: `vnc_init()`+`sync_init()` after `rshell_init()`. `shell.c`: 4 new
  commands (`vnc`, `sync`, `synccfg`, `clipsync`) + a **Cloud:** help row; header
  bumped to **v30.0 / Phase 45 / 126 commands**; the stale `shell_run` welcome
  banner `v20.0` → `v30.0`. `build.bat`: links `vnc.o`/`sync.o` and forwards
  hostfwd `5900`/`7070` (alongside the existing `8080`/`2323`).

### Bug found & fixed during testing
- **Sync/VNC server didn't re-listen after the first client.** First cut only
  re-armed the listener when the TCB went fully inactive (`!c->active`), but a
  client disconnect leaves the TCB in FIN_WAIT (still `active`) — so the *second*
  connection was RST'd by QEMU's NAT (no listener). Fixed both servers to detect
  `c->closed || state != ESTABLISHED`, `tcp_close` our side, and **immediately
  re-listen** (`sync_relisten`/`vnc_relisten`). Re-verified: two consecutive VNC
  captures and repeated sync ops all succeed. (Also caught the text-shell
  servicing gap that motivated the idle hook — a backgrounded server needs the
  poll pumped while `keyboard_getchar` blocks.)

### Verification artifacts
- New tools (host side, not linked): `vnc_capture.py` (minimal RFB 3.3 client →
  PNG), `sync_client.py` (sync protocol client). Screenshots captured *through
  the VNC server itself* (reads real `fb_back`): `p45_vnc_*.png`,
  `p45_sync_guest.png`, `p45_settings_load.png`.

---

## Session — Phase 44: Security  *(NOT committed yet, 2026-06-14)*

Implemented **Phase 44** (Era 6): real authentication, Unix-style file
permissions, cooperative process ownership, and a packet-filter firewall. Two
new modules (`users.c/h`, `firewall.c/h`) plus changes across login, vfs/ramfs,
process, ip, and shell. Kernel 625 KB → 638 KB (under the ~720 KB ceiling); no
new warnings.

### Authentication (`users.c/h`)
- An in-memory user DB (root uid 0, guest uid 1000) with **salted password
  hashing** — an integer-only FNV-1a + 64-round strengthening KDF with a 32-bit
  salt and a two-word digest. **Deliberately NOT CRC32** (which exists in pkg.c
  but is a non-cryptographic checksum) and not SHA/bcrypt (need 64-bit/libgcc).
  Documented as teaching-grade: salted + iterated, but not production crypto.
- `login_run()` now does a **real credential check** and loops until valid
  (was `(void)password;` — accepted anything). It returns the authenticated uid
  and sets the current user. `lockscreen` verifies against the logged-in user's
  hash (was "any non-empty password unlocks").
- "Current user" tracked in `users.c` + mirrored to `process_t.uid`.

### File permissions + ownership
- Added `uid`/`gid`/`mode` to `fs_node_t`. `ramfs_create` sets defaults via
  `vfs_init_perms` (owner = creator, 0644 file / 0755 dir). Boot files end up
  root-owned (boot runs as root).
- Permission checks at the VFS choke points: `vfs_read`/`vfs_write` enforce
  rwx for the current user (root bypasses). `ramfs_delete` gates on ownership.
  `vfs_chmod`/`vfs_chown` with owner/root rules; `vfs_mode_string` renders
  `drwxr-xr-x`.
- Shell: `chmod <octal> <file>`, `chown <user> <file>`, `ls -l` (mode/owner/
  size columns), and `rm` now reports permission denial distinctly.

### Cooperative process isolation (scoped — see note)
- Added `process_t.uid` (inherited from parent / logged-in user) and
  `process_terminate_as(pid, caller_uid)` — only the owner or root may kill a
  process; pid 1 (shell/init) is extra-protected. `cmd_kill` uses it and
  reports "Permission denied". **NOTE: this is cooperative security, not memory
  isolation.** NexusOS runs every process in ONE shared address space, ring 0,
  with a single global page directory (no per-process CR3). Real address-space
  isolation is a 2–3 day paging rewrite (per-process page dirs, CR3 swap in
  context_switch, ring-3, per-process TSS ESP0) — deliberately deferred; the
  uid gates *who may act on* a process, which is the honest, shippable slice.

### Firewall (`firewall.c/h`)
- A stateless, first-match rule list (≤32 rules) matching on direction/proto/
  remote-IP/port, with a master switch and default policy. Hooked at the IP
  layer: ingress in `ip_handle_packet` (after validation, before demux),
  egress in `ip_send_packet` (before frame build). Disabled by default; adding
  a rule auto-enables it. Drop/pass counters.
- Shell: `firewall`/`fw` — `list`, `on|off`, `default <accept|drop>`,
  `block|allow <proto> <ip|any> [port] [in|out]`, `del <#>`, `clear`. Mutation
  is root-only; `list` is open.

### Wiring
- `kernel.c`: `users_init()` before `login_run()`; `firewall_init()` after the
  TCP/IP stack. Shell process's uid set from the login result.
- `shell.c`: 8 new commands (`id`, `users`, `passwd`, `useradd`, `userdel`,
  `chmod`, `chown`, `firewall`); `whoami` now reports the real user (not the
  USER env var); `kill`/`rm`/`ls` upgraded; **Security:** help row; header
  bumped to **v29.0 / Phase 44 / 122 commands**. `build.bat` links
  `users.o`/`firewall.o`.

### Bug found & fixed during testing
- **`chmod 000` didn't deny reads at first.** `vfs_check_perm` had a `mode == 0
  → allow` escape hatch (meant to spare pre-perm-system nodes), but `chmod 000`
  sets mode to exactly 0, so a deliberately-locked file read as world-readable.
  Removed the hatch (every real node now gets a non-zero default from
  `vfs_init_perms`, so mode 0 genuinely means "no access for non-owners"), and
  fixed the parallel `mode != 0` guard in `ramfs_delete`. Re-verified: `cat` on
  a 000 file now returns nothing.
  *(Also: a test-harness gotcha — the `-` in `ls -l` was being dropped by the
  driver, masking the long-format output; not a kernel bug.)*

### Verified live (headless QEMU)
- Wrong password (`root`/`wrongpass`) → **rejected, re-prompts** (was: logged in).
- `guest`/`guest` logs in; `whoami`=guest, `id`=uid=1000(guest)[standard user].
- As guest: `useradd`/`firewall on` → "Permission denied: only root…".
- `chmod 000 mine` then `cat mine` → **no output** (read denied); `ls -l` shows
  `----------  guest  …  mine` vs boot files `-rw-r--r--  root  …`.
- As root: `firewall block icmp any out` then `ping 10.0.2.2` → **4 sent, 0
  received, 4 lost**; `firewall list` → **dropped=4 passed=0**. Real filtering.
- One boot hit the **known intermittent boot double-fault** (eip=0x8, memory
  `intermittent-boot-doublefault`); a clean reboot came up fine — pre-existing,
  not a Phase 44 regression.
- Screenshots: `screenshots/security-file-perms.png`, `security-firewall.png`.

---

## Session — Phase 43: Accessibility  *(NOT committed yet, 2026-06-14)*

Implemented **Phase 43** (Era 6 — Polish & Superiority opener): a full
accessibility layer — all four roadmap pillars — built on top of subsystems
that already existed (the PC speaker, the theme engine, the multi-size font
engine, and the Phase 42 raw keyboard hook). No new boot regressions; kernel
grew ~620 KB → 625 KB (well under the ~720 KB ceiling).

### New module — `kernel/accessibility.c/h`
- **Screen reader via PC speaker** (`speaker.c` `beep`, no FPU): per-event
  *earcons* — a small table of one/two-tone PIT square-wave signatures for
  focus/activate/boundary/error/open/close/toggle-on/off — and `acc_say()`,
  which audibly "spells" arbitrary text by mapping each character to a stable
  pitch (letters on a 2-octave rising scale, digits a high register, spaces a
  brief rest), bracketed by rising/falling chirps. A master switch
  (`accessibility_set_reader`) makes `acc_event`/`acc_say` no-ops when off so
  the rest of the system can call them unconditionally.
- **High-contrast theme**: a 5th theme `"hicon"` (white/yellow on black, every
  pair a max-luminance-difference combo) added to `theme.c`; `theme.h`
  `THEME_HICON`/`THEME_COUNT 5`. `accessibility_toggle_contrast()` swaps to it
  and restores the previous theme. `wallpaper.c` now paints a **plain black
  field** instead of the patterned wallpaper while hicon is active (the VESA
  wallpaper has its own RGB pattern renderer that otherwise ignores the theme
  bg — so high contrast now covers the desktop background too).
- **Font scaling**: a global 1×–4× scale (`font_set_scale`/`font_get_scale`)
  added to the font engine. The non-scaled draw/measure helpers
  (`font_draw_char`/`string`/`_transparent`/`_aa`, `font_measure_string`) honor
  it by delegating to the existing `_scaled` renderers, so **free-positioned**
  UI text (desktop icon labels, window titles, widget labels, taskbar)
  enlarges. **Fixed character-grid surfaces stay 1×** — the raw VESA text
  console (`vga.c`) and the GUI text-cell renderer (`gui.c`, used by the
  Terminal/Notepad windows) draw on a fixed `col*W`/`row*H` grid, so a >1×
  global scale would render oversized glyphs on a 1× grid and overlap into
  garbage. They use a new `font_draw_char_fixed()` (always 1×, ignores the
  global scale). Explicit `*_scaled()` callers (game/HUD code) are unaffected;
  `font_draw_char_scaled(scale=1)` also routes to `_fixed`. `gfx_set_font_scale`
  wrapper added. (Scaling grid *content* — terminal/notepad body text — would
  need a column-reflow rewrite; deferred. Chrome/labels reflow naturally.)
- **Keyboard-only nav + global hotkeys**: accessibility takes the keyboard's
  single raw-hook slot and **chains** the gamepad — the pad hook was refactored
  into `gamepad_raw_observe()`, which the accessibility hook forwards every
  scancode to. Global chords (fired in IRQ context, **state-only** — `beep()`
  blocks so it's never called from the IRQ): `Alt+Shift+C` toggle contrast,
  `Alt+Shift+S` toggle reader, `Alt+Shift+=`/`-` text larger/smaller,
  `Alt+Shift+A` announce status. The deferred earcon is drained by
  `accessibility_poll()`, called once per frame from `desktop_run()` (which
  also redraws when a hotkey changed the theme/scale). While a game owns the
  pad, the chord check yields (the pad consumes the key first).

### Wiring
- `kernel.c`: `accessibility_init()` after `speaker_init()` (earcons need the
  speaker) and after `gamepad_init()` (we chain its hook).
- `shell.c`: 5 commands (`accinfo`, `fontsize <1-4>`, `contrast [on|off]`,
  `reader <on|off>`, `say <text>`); help gets an **Access:** row; header bumped
  to **v28.0 / Phase 43 / 114 commands** and the stale help banner `v18.0` →
  `v28.0`. `theme` command help now lists `hicon`.
- `desktop.c`: includes + the per-frame `accessibility_poll()` and
  theme/scale-change redraw trigger.
- `build.bat`: links `accessibility.o` (compile step auto-globs `kernel\*.c`).

### Pre-existing bug fixed in passing
- **`appearance.c` theme panel never applied a theme**: it called
  `theme_set_by_name(theme_names[ap_sel])` with display-capitalized names
  ("Dark"), but `theme.c` stores lowercase canonical names ("dark") and
  `strcmp` is case-sensitive — so every selection silently failed. Switched to
  `theme_set(ap_sel)` by index (the arrays are index-aligned), which also makes
  the new HighContrast entry selectable from the panel.

### Verified live (headless QEMU, virtio-gpu path)
- Boots clean to login through the new init (no regression).
- `accinfo` lists reader/contrast/scale state + hotkeys + commands; `reader`
  toggles to ON; `say hello` prints "Speaking: hello" and **returns** (proving
  the per-char beep loop ran h-e-l-l-o without hanging).
- Desktop with `contrast on` + `fontsize 2`: **black field**, **2× icon
  labels**, themed window chrome.
- Global hotkeys in the desktop: `Alt+Shift+-`×2 dropped scale back to 1×,
  `Alt+Shift+C` turned contrast off — desktop redrew correctly.
- `help` shows the **Access:** row, renders clean (no white-block).
- Regression: `doom` title → START → W moved the player (gamepad hook chain
  intact; pad still consumes keys when a game owns it).
- Screenshots: `screenshots/accessibility-highcontrast.png`,
  `screenshots/accessibility-accinfo.png`.

---

## Session — Phase 42: Gaming Framework  *(NOT committed yet, 2026-06-12)*

Implemented **Phase 42** (Era 5 — Multimedia finale): "NexusSDL", an SDL-like
gaming framework with a 12-button virtual game controller, plus **NEXUSDOOM**
(a from-scratch textured raycaster FPS) and Breakout as API demos. Also broke
the 576 KB kernel-size ceiling with a chunked high-load bootloader rework, and
root-caused and fixed two long-standing display bugs.

### Phase 42 — Gaming Framework
- New: `kernel/gamepad.c/h` (virtual controller), `kernel/game.c/h` (NexusSDL),
  `kernel/doom.c/h` (NEXUSDOOM), `kernel/breakout.c/h` (Breakout).
- **Controller layer**: a raw-scancode hook added to the PS/2 driver
  (`keyboard_set_raw_hook`) feeds per-key held state and a 12-button virtual
  pad (D-pad/A/B/X/Y/L/R/Start/Select, each with up to 3 source keys, e.g.
  UP = Up-arrow or W). Held levels, latched per-frame edges, and an SDL-style
  event ring. Games `gamepad_acquire()` (keys stop reaching the console) and
  `gamepad_release()` on exit (drains stray buffered chars).
- **NexusSDL** (`game.c`): 256-color palettized surface up to 320×240 with
  RAMP palette (15 ramps × 16 shades), pixels/lines/rects/circles/blits/scaled
  8×8 text, Q16.16 fixed-point math kit (`fx_mul` via widening `imull`,
  `fx_div` via inline `idivl` — no libgcc), LUT trig (Bhaskara, 1024-unit
  circle), tick-locked `game_sync()` (~18 fps), non-blocking PC-speaker sfx.
  `game_present()` palette-expands + integer-scales to the back buffer and
  presents via `gpu_present` dirty rect (VESA flip fallback).
- **NEXUSDOOM** (`doom.c`): 24×24 world, DDA raycaster (one wall walk per
  column, perpendicular distance, per-column z-buffer), 4 procedural 64×64
  textures + glowing EXIT door, 6 chasing/meleeing billboard imps z-tested
  against walls, hitscan pistol with recoil/muzzle flash, HUD, minimap (V),
  title/play/dead/win states. Clear all demons then bump the exit to win.
- **Breakout** (`breakout.c`): 6×10 bricks, Q16 ball physics, paddle english,
  lives/score — the "look how little code a game needs" demo (~200 lines).
- Shell: `gameinfo`, `gamepad` (visual controller tester), `doom`, `breakout`
  — now **109 commands** (shell v27.0). `kernel.c`: `gamepad_init()` +
  `game_init()` after `gpu_init()`. `build.bat`: 4 new objects.

### Bootloader — chunked high load (576 KB ceiling broken)
- The kernel hit 619 KB and the old in-place real-mode load at 0x10000 ran
  into the VGA hole at 0xA0000 (hard cap 576 KB) — boot died with a black
  screen. Reworked `boot2.asm`: BIOS-reads 32 KB chunks into a bounce buffer
  at 0x10000, then **hops into protected mode per chunk** to `rep movsd` it up
  to 1 MB (new 16-bit code/data GDT entries 0x18/0x20 for the PM→real return).
  `linker.ld`: kernel base 0x10000 → **0x100000**; capacity now ~720 KB.
- **Gotcha found the hard way**: stage2 now extends past 0x8000, and the VBE
  `ModeInfoBlock` scratch buffer at `0x8000` silently corrupted the GDT
  (diagnosed via monitor `info registers` showing garbage GDTR + live-memory
  hexdumps vs the on-disk image). Moved the scratch buffer to **0x0500**.

### Bug fixes (both long-standing, root-caused this session)
- **White-block console artifact — FIXED**: the "cosmetic" white rectangle
  after heavy console scrolling (top-left 640×384 going solid white on `help`)
  was real: the text console kept its character model at legacy **0xB8000**,
  but once the Phase 41 VirtIO-GPU scanout activates, QEMU unmaps the
  VGA-compat window — writes are discarded, **reads return 0xFF** — so every
  scroll redraw read back 0xFFFF cells (char 0xFF, white-on-white) and painted
  the whole text area white. Fix (`vga.c`): the text model now lives in a
  kernel-RAM shadow array; 0xB8000 is only a write-through target in text mode.
- **8×8 font off-by-one — FIXED**: `font8x8.h`'s `0x12-0x1F` block had 13
  entries for 14 codes (missing 0x1F ▼), so every glyph from space onward sat
  one slot low and **all 8×8 text rendered as char+1** ("UP" → "VQ"). Affected
  NexusSDL text, doom/breakout HUDs, and the 8×8 system-font option.
- Also: doom's exit stats mixed scopes (frames since open ÷ seconds since last
  restart → "83 fps"); now timed over the whole session ("287 in 15s = 18 fps").
- **Shell history-recall stranding — FIXED**: `shell_readline` captured only
  the input's start *column* and, on up/down-arrow recall, repositioned to
  `(vga_get_cursor_row(), start_col)` after backspacing the old input. When the
  command's echo had scrolled or wrapped, `vga_get_cursor_row()` no longer
  matched the prompt's row, so the recalled text was painted indented on a
  *lower* row (the stray "helpS" floating mid-screen). Replaced the absolute
  repositioning with a relative `readline_replace()` that just backspaces the
  echoed chars (`vga_backspace` already walks back across row/scroll
  boundaries) and reprints inline — stays glued to the prompt regardless of
  scrolling.

### Verified live (QEMU 9.1, headless monitor driving)
- Boots through the new chunked high-loader to login → desktop → shell.
- `help` (heavy scroll) renders clean — no white block.
- `gamepad`: all 12 boxes render, held keys light up green (sendkey hold),
  HELD/EVENTS/FPS 18 counters live, Esc exits to a summary.
- `doom`: title → play (imp in the crosshair, HUD: HP/AMMO/KILLS/FPS 18),
  fired and **killed an imp (2 shots, 100% accuracy)**, took melee damage,
  death screen on HP 0, Esc exits with correct tick-locked stats.
- `breakout`: bricks break (SCORE 10), serve prompt, lives lost, clean exit.
- Driving note: QEMU `sendkey <key> <hold-ms>` holds work, but **don't overlap
  a long hold with further sendkeys** — serialize with sleeps ≥ the hold time.
- Screenshots: `screenshots/doom.png`, `doom_title.png`, `breakout.png`,
  `gamepad.png`.

---

## Session — Phase 41: GPU Acceleration  *(NOT committed yet, 2026-06-11)*

Implemented **Phase 41** (Era 5 — Multimedia): a VirtIO-GPU driver over a
from-scratch VirtIO 1.0 modern PCI transport, with zero-copy scanout presents,
dirty rectangles, rep-string 2D primitives and a sprite engine.

### Phase 41 — GPU Acceleration
- New: `kernel/virtio.c/h` (transport), `kernel/gpu.c/h` (VirtIO-GPU driver +
  accelerated fill/blit), `kernel/sprite.c/h` (sprite engine).
- **VirtIO transport**: parses the PCI capability list for the vendor virtio
  structures (common/notify/ISR/device cfg), identity-maps the MMIO windows
  (page-mapped, cache-disabled), negotiates `VERSION_1` through the 32-bit
  feature-select windows (no 64-bit math — no libgcc), and runs **split
  virtqueues** in polled mode: place a 2-descriptor chain (cmd out + resp in),
  ring the notify doorbell, poll the used ring with a tick timeout + spin cap.
- **VirtIO-GPU bring-up** (QEMU `-device virtio-vga`, PCI `1AF4:1050`):
  controlq (clamped to 16 entries) → `DRIVER_OK` → `GET_DISPLAY_INFO` →
  `RESOURCE_CREATE_2D` (B8G8R8X8 = our XRGB byte order) → `ATTACH_BACKING`
  pointed **directly at the kernel back buffer** (identity-mapped heap, so
  guest-phys == virt: zero-copy) → `SET_SCANOUT` → first flush. QEMU then
  retires the VGA-compat output and the driver owns the display.
- **Presents**: `fb_flip()` now routes to `gpu_flip()` (one
  `TRANSFER_TO_HOST_2D` + `RESOURCE_FLUSH`) when the scanout is active, and
  falls back to the legacy 3 MB VESA memcpy otherwise (kept as
  `fb_flip_legacy()` for the benchmark). `gpu_present(x,y,w,h)` gives
  dirty-rect presents.
- **2D primitives**: `gpu_fill` / `gpu_blit` (`rep stosl` / `rep movsl`,
  clipped, overlap-safe vertical/horizontal ordering).
- **Sprite engine**: pool of 32 ARGB sprites (≤64×64) with per-pixel alpha
  blending (integer `d+(s-d)*a>>8`), z-ordering, clipping, show/hide;
  `sprite_paint_ball()` paints shaded anti-aliased balls for the demo.
- Shell: `gpuinfo`, `gpubench`, `sprites` — now **105 commands** (shell v26.0).
- `build.bat`: link `virtio.o`/`gpu.o`/`sprite.o`; run/debug QEMU lines now use
  `-vga none -device virtio-vga`. `kernel.c`: `gpu_init()` after `video_init()`.
- **Bug found & fixed during bring-up**: after the first command completed, the
  device latched ISR=3 and virtio's **level-triggered INTx stormed the PIC**
  (the polled driver never read the ISR), freezing boot to a crawl — diagnosed
  via QEMU monitor register sampling + a symbolized `kernel.elf` (stack showed
  `gpu_init → gpu_cmd → virtio_run` frozen for minutes, `info virtio-status`
  showed the handshake complete and `used_idx=1`). Fix: set PCI INTx-disable
  (command bit 10), `VIRTQ_AVAIL_F_NO_INTERRUPT` on the avail ring, and ack the
  ISR (read clears it) after every poll.
- Note: under virtio-vga's VBE, bootloader mode 0x118 comes up **24bpp**
  (pitch 3072) instead of 32bpp — harmless: the back buffer stays 32-bit and
  the GPU scanout bypasses the VESA aperture entirely (the 24bpp conversion
  path covers the pre-scanout boot console).
- **Verified live** (QEMU 9.1, headless monitor driving): boots to login and
  desktop **through the virtio scanout** (VGA compat retired — everything on
  screen is transfer/flush); `gpuinfo` reports 1024×768 B8G8R8X8, backing
  0x500000 zero-copy, VERSION_1; `gpubench`: **4,620 fps full-frame present
  vs 107 fps VESA memcpy (43×)**, 31,777 fps 64×64 dirty rects, 4.1 GB/s
  rep-stosl fills (4× the per-pixel path), 3.1 GB/s blits; `sprites`: 10
  alpha-blended balls at **~520 fps average** (6,355 frames / 12.2 s).
  Fallback re-verified with `-vga std` → "VESA framebuffer fallback", boots
  identically. See `ph41_*.png`, `screenshots/gpu-sprites.png`,
  `screenshots/gpu-bench.png`.

---

## Session — Phase 40: Video Playback  *(NOT committed yet)*

Implemented **Phase 40** (Era 5 — Multimedia): a RIFF/AVI parser + MJPEG media
player with synchronized audio, building on Phase 38 (AC'97) and Phase 39 (JPEG).

### Phase 40 — Video Playback
- New: `kernel/video.c/h` (RIFF/AVI parser + media player) and the generated
  `kernel/sample_video.h` (embedded `demo.avi`). New tool `gen_video.py` writes a
  small MJPEG+PCM AVI by hand (RIFF/hdrl/strl/movi muxer).
- **AVI parser** (`avi_load`): walks `hdrl` (`avih` for size/frames/µs-per-frame,
  per-stream `strh`/`strf` to identify the `vids`/`MJPG` and `auds`/PCM streams)
  and `movi` (collects `NNdc`/`NNdb` video + `NNwb` audio chunk offsets). Does not
  rely on `idx1`. `avi_parse()` exposes metadata for `vidinfo`.
- **Player** (`video_play`): decodes each Motion-JPEG frame via `jpeg_decode`
  (Phase 39), renders it with the new shared `image_present()` (scaled-to-fit +
  caption, no key-wait), and concatenates the PCM stream and plays one frame's
  worth of samples per video frame through `audio_play_pcm` (Phase 38) — so the
  **audio clock paces the video** (A/V sync), with a tick-paced fallback for
  silent clips. Any key stops playback.
- Refactored `image.c`: extracted `image_present()` (render one decoded frame)
  out of `image_view()` so both the image viewer and the video player share it.
- Shell: `vidinfo [file]` (parse + report W×H / frames / fps / codec / audio),
  `mplay [file]` (play; no arg = embedded `demo.avi`). Now 102 commands.
- `build.bat`: added `video.o` to the link list. `kernel.c`: `video_init()`.
- **Verified live**: `vidinfo` reported the demo as `MJPG 80x60, 16 frames,
  8 fps, audio 8000 Hz 1ch 16-bit, 16 chunks` (parser correct). `mplay` played
  the clip — captured frames 11/16 and 16/16 show the bouncing ball animating
  and colours cycling (real per-frame MJPEG decode), caption
  `demo.avi 80x60 frame N/16 8 fps +audio`, then returned cleanly. Captured the
  AC'97 output with QEMU's `wav` backend → **70.1% non-zero samples** over the
  playback, confirming synced audio actually played. See `ph40_*.png` and
  `screenshots/video-player.png`.

### Notes
- The demo `demo.avi` (~57 KB) is embedded in the kernel (`sample_video.h`) since
  it exceeds the 4 KB RAM-FS file cap; `mplay`/`vidinfo` with no arg use it, and
  with a filename they read from the VFS.
- Video frames decode to the same ARGB pipeline as still images; "raw frame
  rendering" = decoded MJPEG → framebuffer via `image_present()`.

---

## Session — Phase 39: Image Formats  *(NOT committed yet)*

Implemented **Phase 39** (Era 5 — Multimedia): BMP/PNG/JPEG/GIF decoders + a
fullscreen viewer.

### Phase 39 — Image Formats
- New: `kernel/image.c/h` (core: magic-byte detect, BMP decoder, dispatch,
  fullscreen viewer, boot-time sample installer), `kernel/png.c`,
  `kernel/jpeg.c`, `kernel/gif.c`, and the generated `kernel/sample_images.h`.
  New tool `gen_images.py` (Pillow) emits the embedded samples.
- **Unified pipeline**: `image_decode()` detects the format by magic bytes and
  dispatches to a per-format decoder, each producing a heap `0x00RRGGBB` buffer.
  Integer-only math throughout (no FPU/libm/64-bit division). Dimensions capped
  at 256² to keep heap allocations modest (the layout landmine).
- **BMP** — uncompressed 24/32-bit, top-down + bottom-up.
- **PNG** — a from-scratch **DEFLATE/zlib inflater** (stored + fixed + dynamic
  Huffman, puff-style canonical decode) + all five scanline filters
  (none/sub/up/avg/paeth); colour types 0/2/3/4/6 at 8-bit, non-interlaced.
- **JPEG** — **baseline** sequential: DQT/DHT/SOF0/DRI/SOS, canonical Huffman,
  dequant, integer **IDCT** (NanoJPEG-style), chroma upsampling for any sampling
  factor, YCbCr→RGB. Grayscale + 3-component.
- **GIF** — GIF87a/89a, **LZW** (variable code width, clear/EOI), global/local
  colour tables, interlacing, transparency, **multi-frame** compositing with
  disposal; `gif_decode_frame()` drives animation in the viewer.
- **Viewer**: `image_view()` clears the framebuffer, nearest-neighbour scales the
  image to fit 1024×768, draws a caption, `fb_flip()`, waits for a key; animated
  GIFs cycle frames (polling `keyboard_has_key()`).
- Shell: `imginfo` (decodes all 4 samples, prints format/size/frames/centre
  pixel), `view <file>`. Now 100 commands. Samples `icon.bmp` / `logo.png` /
  `photo.jpg` / `anim.gif` installed into the RAM-FS at boot (each < 4 KB).
- `build.bat`: added `image.o`/`png.o`/`jpeg.o`/`gif.o` to the link list.
- **Verified live** by comparing each decoded centre pixel against the reference
  emitted by `gen_images.py`: BMP `0xF0F0F0` ✓, PNG `0x28283C` ✓ (exact —
  inflate + filters correct), JPEG `0x828282` ✓ (baseline IDCT), GIF `0x181820`
  with `frames=3` ✓. `view` renders each correctly (PNG disc, JPEG gradient with
  DCT smoothness, GIF animation cycling) — see `ph39_*.png` and
  `screenshots/image-viewer.png`.

### Notes
- Decoders test against **real Pillow encoder output** (dynamic-Huffman PNG,
  standard LZW GIF, libjpeg baseline) — a stricter test than hand-crafted files.
- `view <file>` requires the argument inside one quoted `qemu_drive.py` token
  (e.g. `'type:view logo.png'`); unquoted, the space-split drops the arg and the
  command falls back to its default file.

---

## Session — Phase 38: Sound (Era 5 begins)  *(NOT committed yet)*

Started on **Phase 38** (first phase of Era 5 — Multimedia); the Sound Driver.

### Phase 38 — Sound Driver
- New: `kernel/audio.c`, `kernel/audio.h` (device-agnostic core) and
  `kernel/ac97.c`, `kernel/ac97.h` (Intel AC'97 driver). Mirrors the
  `net.c` (core) + `rtl8139.c` (driver) split.
- **Intel 82801AA AC'97 driver** (QEMU `-device AC97`, PCI `8086:2415`):
  probes PCI (exact id, else class `04:01`), enables I/O + bus-master, brings
  the controller out of cold reset, resets the codec, waits for primary-codec-
  ready. Reads the **NAM (mixer)** and **NABM (bus-master)** I/O windows from
  the device's two BARs.
- **Bus-master PCM-out** via a **Buffer Descriptor List**: per chunk it resets
  the PCM-out engine, points BDBAR at a 16 KB DMA buffer (from
  `kmalloc_aligned`, heap is identity-mapped <16 MB so virt==phys), sets LVI,
  runs (RPBM), and polls SR `DCH` to completion — bounded by a tick timeout +
  spin cap so a wedged engine can't hang the kernel. Poll-mode (no IRQ armed).
- **Software mixer** (`audio.c`): up to 8 PCM voices summed into 16-bit stereo,
  per-voice + master volume (0–100), mono→stereo up-mix, nearest-neighbour
  resampling (Q16 accumulator) to the 48 kHz device rate.
- **WAV player**: RIFF/WAVE parser (8/16-bit, mono/stereo PCM); 16-bit borrowed
  in place, 8-bit converted. A `startup.wav` chime (8 kHz mono, sized to fit the
  4 KB RAM-FS file cap) is generated + installed at boot.
- **Tone generator**: integer-only sine (Bhaskara approx + Q16 phase) with small
  looping waveform buffers — no FPU, no 64-bit division (freestanding kernel).
- Shell: `sndinfo`, `play <file.wav>`, `volume <0-100>`, `tone <hz> <ms>`,
  `mixer` (3-voice C-E-G chord demo). Now 98 commands.
- `build.bat`: added `audio.o`/`ac97.o` to the link list and
  `-audiodev dsound,id=snd0 -device AC97,audiodev=snd0` to the QEMU run/debug
  lines.
- **Verified live**: `sndinfo` shows the codec ready (NAM `0xC000` / NABM
  `0xC500`); `play startup.wav` parses + plays (8 kHz→48 kHz resample); `mixer`
  blends 3 voices; `tone` works. Captured the AC'97 output with QEMU's `wav`
  audiodev backend → **98.9% non-zero samples, peak 28632/32767 over 2.83 s** of
  active playback, i.e. real PCM made it through mixer→BDL DMA→device→host.

### Notes
- Early iteration used full-duration per-voice buffers (mixer alloc'd ~288 KB);
  reworked `tone`/`mixer` to **small looping buffers** (~few KB) to avoid
  aggravating the pre-existing heap/stack/`fb_back` layout landmine (see Known
  issues). One run hit a mid-session lockup consistent with that latent bug;
  after the rework all runs were clean and the CPU stays idle between commands.
- A transient **white block** can appear in the framebuffer text console after
  heavy scrolling — purely cosmetic (cleared instantly by `clear`); not a hang
  and unrelated to audio.

---

## Session — Phases 35→37 + UI polish

Started with the project on **Phase 35**; ended on **Phase 38** (Era 4 complete).

### Phase 35 — Package Manager  *(committed + pushed)*
- New: `kernel/pkg.c`, `kernel/pkg.h`.
- `.npk` binary archive format: packed header (`NPK1` magic) + dependency table
  + file table + payload, with a **header CRC32 over the whole body and a
  per-file CRC32**.
- Bundled repository of 8 packages (`libnx`, `coreutils`, `hello`, `fetch`,
  `nano`, `nxedit`, `sdl-shim`, `doom`) with a real dependency graph.
- **Recursive dependency resolution** (depth-first, deps first) with cycle
  detection, deduplication, and minimum-version (`>=`) checks.
- Install pipeline: `serialize → CRC verify → extract into VFS → register`,
  with all-or-nothing rollback on filesystem failure. Installed database tracks
  files/sizes so `npkg remove` cleans up precisely.
- Shell: `npkg list/search/info/install/remove/installed/update`.
- Verified live in QEMU: `npkg install doom` pulled in `libnx`+`sdl-shim` in
  correct order; files extracted to the VFS (`ls`/`cat` byte-exact); `remove`
  cleaned up while keeping shared deps.

### Phase 36 — Scripting Engine (NexusScript)  *(committed + pushed)*
- New: `kernel/script.c`, `kernel/script.h`.
- Real interpreter: lexer → token array → recursive-descent evaluator walked by
  a token cursor (no AST, heap-light).
- Language: int/string values, `let`/reassignment, `if/elif/else/end`,
  `while/do/end`, `print`, `#` comments; operators `+ - * / %`,
  `== != < <= > >=`, `and/or/not`; builtins `len/str/abs`; `run "<cmd>"` runs a
  shell command (OS automation).
- **Safety:** per-statement/iteration step budget so a runaway script can't hang
  the kernel; recursion + `run "script ..."` nesting are bounded.
- Added public `shell_exec_line()` in `shell.c`/`shell.h` so `run` can drive the
  shell. Shell: `script <file.ns>`; a sample `demo.ns` is installed at boot.
- Verified: `script demo.ns` exercised arithmetic, `str()`, concat, `while`,
  `if/else`, and `run "ls"`; bad scripts report `line N: <error>` and recover
  without crashing.

### Phase 37 — macOS Compatibility Shim  *(NOT committed yet)*
- New: `kernel/macho.c`, `kernel/macho.h`, `kernel/cocoa.c`, `kernel/cocoa.h`.
- **Mach-O i386 loader** (mirrors the PE loader): validates
  `MH_MAGIC`/`CPU_TYPE_X86`/`MH_EXECUTE`, walks load commands (`LC_SEGMENT`,
  `LC_UNIXTHREAD`/`LC_MAIN`, `LC_LOAD_DYLIB`), maps segments (skips
  `__PAGEZERO`), resolves the entry, transitions to ring 3.
- **Core Foundation**: reference-counted object pool
  (`CFRetain`/`CFRelease`/`CFGetRetainCount`), `CFString`, `CFNumber`, `CFShow`.
- **Cocoa/AppKit**: `NSWindow` bridged to the NexusOS window manager,
  `NSString`, `NSLog`; releasing an `NSWindow` tears down its WM window.
- Shell: `machoinfo`, `runmacho <file>`, `cocoademo`.
- Verified: `machoinfo` shows status; `cocoademo` builds CF objects and a live
  `NSWindow` that renders on the desktop (bridged to the WM).

### UI polish (not a roadmap phase)  *(NOT committed yet)*
- **macOS window chrome** (`kernel/window.c`): traffic-light buttons on the
  LEFT (🔴 close / 🟡 minimize / 🟢 maximize, glyphs on focus, grey when
  unfocused), centered title, shared `win_btn_left()` helper so the clickable
  hit-zones can't drift from what's drawn.
- **Terminal redesign** (`kernel/desktop.c`): `nexus@os:~$` multi-color prompt,
  banner, dimmed command echoes, red error lines, blinking block cursor, larger
  window, role-based line coloring.
- Discovered the ~26 GUI apps already share a design system (`kernel/appui.c/h`,
  header bars / panels / accent colors), e.g. the File Manager is a Finder-style
  three-pane browser. They already match the new chrome.

---

## Known issues

- **Intermittent boot double-fault** (pre-existing, NOT from the above work):
  unhandled `#DF` (ISR 8) early in boot, serial `!EX:00000008 ... 00000008`
  (jump to near-null `eip=0x8`). The **same binary boots fine on most runs** →
  timing/layout-sensitive, likely the timer-driven scheduler racing the
  `kernel_main` init sequence. Suspect memory map: kernel stack is `0x400000`
  while the heap range is `0x200000..0x800000` and `fb_back` is `0x500000`
  (stack + framebuffer back-buffer sit inside the heap's address range; BSS was
  already relocated to `0x800000+` to dodge an earlier overlap). If a boot
  faults, just rebuild/relaunch. A real fix = give the kernel stack its own
  region outside the heap and/or defer scheduler preemption until init finishes.

---

## Build & test quickref

- Build (Windows): `cd NexusOS && .\build.bat` (toolchain under `NexusOS/tools/`).
  New `kernel/*.c` files must be added to the link list in `build.bat`
  (the Makefile auto-globs).
- Drive in QEMU: launch with `-monitor tcp:127.0.0.1:55555,server,nowait
  -serial file:serial.log`, then `python qemu_drive.py` sends `type:`/`ret`/
  `key:`/`sleep:`/`shot:` actions. Login accepts any user/pass; press **Esc** on
  the desktop to reach the text shell.
- Crashes are written to `serial.log` as `!EX:<int_no> <err_code> <eip>`.

## Git

- Remote `origin` = https://github.com/rajvveer/nexusOs (branch `main`).
- Pushed this session: `aacd2d9` (Win32/pkg/script, Phases 34–36) and `7236194`
  (README screenshots).
- **Not yet committed:** Phase 37 (macho/cocoa) and the UI polish
  (window chrome + terminal redesign + screenshots refresh).
