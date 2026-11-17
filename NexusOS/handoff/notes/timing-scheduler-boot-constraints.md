---
name: timing-scheduler-boot-constraints
description: NexusOS PIT is default 18.2Hz (whole codebase assumes ticks/18=sec — do NOT reprogram); scheduler exists but is intentionally disabled (cooperative); the boot
metadata: 
  node_type: memory
  type: project
  originSessionId: 8354ac3a-d200-4aba-9b6f-a9b615d04b0a
  modified: 2026-10-08T07:33:10.981Z
---

**UPDATE 2026-10-08 (Phase 54): the scheduler is now ENABLED** (`sched_set_enabled(1)` in uproc_init) — but it preempts ONLY ring-3 user processes; the kernel/desktop thread is never pre-empted (its sole hand-off is `sched_yield()` in the shell idle pump), so the lock-free kernel state stays safe. The PIT is still 18.2 Hz (unchanged). See [[phase54-isolation-usb-folders]].

Three load-bearing facts confirmed in Phase 48 (Performance Optimization):

- **PIT is the DEFAULT 18.2065 Hz — channel 0 is NEVER reprogrammed** (only
  channel 2 for the PC speaker, in game.c). The ENTIRE codebase hardcodes
  `system_ticks/18 = seconds` and `*55 ms`: STATUSBAR_INTERVAL=18, TICK_MS=55,
  TIMESLICE=5, DHCP=90, DNS=36, uptime, doom 18fps, audio pacing, Win32
  GetTickCount. **Do NOT reprogram the PIT** without auditing ~dozens of files —
  it's a landmine. Timing resolution is ±55 ms (so "boot < 1s" is coarse).

- **The round-robin scheduler is intentionally DISABLED.** `scheduler_start()`
  exists (scheduler.c) but is NEVER called → `schedule()` is a no-op; the system
  is one cooperative thread. Do NOT enable it: the kernel is one shared address
  space with NO locks on heap/VGA/VFS/net rings (see
  [[nexusos-single-address-space]]), so preemption would corrupt them
  mid-syscall, and context_switch would jump to processes with uninitialized
  esp. SMP is further out still. The `smp`/`preempt` shell commands document
  this honestly — keep it that way unless someone does the full locking + per-CPU
  rewrite.

- **The intermittent boot double-fault (#DF, eip=0x8) is an IRQ-during-init
  race.** `idt_load` (idt_asm.asm) does `sti` BEFORE `pic_init` unmasks IRQs and
  before the rest of init runs, so IRQ0 fires while pmm/heap/rtc/VGA/net are
  being built; the timer handler's status-bar redraw (reads rtc/pmm/VGA) +
  net_poll mid-init is the trigger. Phase 48 added a `volatile bool
  boot_init_done` guard in kernel.c: `system_ticks++` stays unconditional, the
  rest of timer_callback returns early until init finishes. The PIC EOI is sent
  by irq_handler BEFORE the callback, so the early return is PIC-safe. This is a
  MITIGATION; the DEFINITIVE fix (deferred) is to move `sti` out of `idt_load`.
  Supersedes the "just rebuild if it faults" advice in
  [[intermittent-boot-doublefault]]. **CORRECTION 2026-10-08:** the guard was
  ineffective — the fault was IRQ0 arriving at un-remapped BIOS vector 8 before
  `pic_init` (~14% of boots). Now FIXED by masking the PIC in boot2 and moving `sti`
  after `pic_init`; see [[intermittent-boot-doublefault]].

Also Phase 48: `memcpy`/`memset` are now `rep movsl`/`rep stosl` (aligned fast
path + byte tail), overlap-safe (only tcp.c overlaps, dst<src forward-safe; a
backward branch covers dst>src). No `memmove` exists. ~2.9 GB/s copy / 3.2 GB/s
fill. Size ceiling (2026-10-08): boot2 KERNEL_SECTORS=1984 -> kernel.bin must stay <= 1,015,808
bytes (build.bat now fails the build past that); `.eh_frame` is discarded in linker.ld.
Aurora build is ~764 KB. See [[shell-command-introspection]].
