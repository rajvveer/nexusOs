---
name: phase54-isolation-usb-folders
description: "Phase 54 (2026-10-08) — per-process address spaces + ring-3 user procs + enabled scheduler, real UHCI USB stack, ramfs directories, authenticated rshell/sync. Built by 3 parallel subagents in worktrees, merged by hand."
metadata:
  node_type: memory
  type: project
  originSessionId: 37d1b0eb-50a3-46b5-aea1-6f0971a47e47
  modified: 2026-10-08T13:56:39.335Z
---

On 2026-10-08, after the Aurora UI, the user asked to also add: memory isolation,
USB, a directory filesystem, and auth for the network services. Done by 3 subagents
each in a copied worktree (scratchpad/ws_core, ws_usb, ws_fs), then merged into the
real repo with `git merge-file` (only shell.c and desktop.c needed conflict resolution;
each agent's other files were real==base so copied directly). Full merge builds clean
at ~792 KB, **0/15 boot crashes**, all four features verified together.

**Process isolation + scheduler (ws_core).** New `uproc.c/h`: each loaded binary runs in
its OWN page directory (kernel space + LFB mapped shared/supervisor; user image + stack
mapped PAGE_USER in the process's own tables; CR3 switched on context switch). Ring 3,
syscalls via `int 0x80`. **The scheduler is now ENABLED** (`sched_set_enabled(1)` in
`uproc_init`) but preempts **only ring-3 user processes** — the kernel/desktop thread stays
cooperative (its one hand-off is `sched_yield()` in the shell idle pump; never pre-empted),
so the lock-free kernel state is safe. A user fault (e.g. ring-3 read of a kernel address)
kills just that process via the page-fault handler (`idt.c`) and prints "Segmentation fault
(pid N, addr ...)"; the OS survives. Shell: `runelf <file> [&]` (uproc_run_path), `ps` (now
with a UID column + user procs), `kill`. Demo bins embedded in `usertests.h` (hello/spin/segv
in ELF/PE/Mach-O), installed into ramfs at boot by `uproc_init`; regenerate with
`gen_usertests.py`. **This supersedes [[nexusos-single-address-space]] and the
"scheduler disabled" note in [[timing-scheduler-boot-constraints]].**

**USB (ws_usb).** Real UHCI driver in `usb.c` (PIIX3 via PCI 0C/03/00, I/O BAR4, frame
list + QH/TD in kmalloc_aligned identity-mapped DMA memory, fully polled — no USB IRQ).
Enumeration is descriptor-driven (class NOT hardcoded). `usb_hid.c`: boot-protocol kbd/mouse,
polled from the shell idle pump + desktop loop + login loop (never an IRQ), feeds
`keyboard_inject_char`/`mouse_inject`. `usb_storage.c`: Bulk-Only Transport + SCSI
READ/WRITE(10), registers as blkdev `usb0`. Shell `usbinfo`/`lsusb`. **Test with QEMU
`-device piix3-usb-uhci` + `usb-kbd`/`usb-mouse`/`usb-storage`** (UHCI has only 2 root ports;
>2 devices makes QEMU insert an auto-hub which this driver doesn't walk — test device sets
separately). usb_init() is the old stub's entry point in kernel.c (unchanged), now real.

**Directories (ws_fs).** ramfs now has real FS_DIRECTORY trees (parent/children/next in
fs_node_t, previously unused). `ramfs_mkdir/rmdir/resolve/abspath`; default `/home`,
`/home/<user>`, `/tmp` at boot. Shell + GUI-terminal: `cd`/`mkdir`/`rmdir`/`pwd`/`ls [path]`
path-aware against a cwd; `cat`/`touch`/`rm`/`write` + `>`/`>>` take paths. Flat-root files
still live in `/`.

**Remote auth (ws_fs).** rshell (2323) and sync (7070) now require a `LOGIN <user> <pass>`
(or `USER`/`PASS`) handshake checked via `users_authenticate`, then run requests under that
uid. In-OS sync client auto-logs-in (sync_set_login, default root/root). sync_client.py
updated (`--user`/`--pass`). httpd (8080) is view/info-only (no gate). **vnc (5900) RFB
type-2 DES auth is still a documented stub** — off by default, operator-only.

See [[aurora-ui-architecture]], [[headless-test-harness-gotchas]] (F12 to console),
[[nexusos-1080p-memory-layout]].
