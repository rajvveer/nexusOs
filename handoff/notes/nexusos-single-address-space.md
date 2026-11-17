---
name: nexusos-single-address-space
description: NexusOS runs every process in ONE shared address space, ring 0, no per-process paging — "isolation"/sandboxing is cooperative only until a paging rewrite
metadata:
  type: project
---

**UPDATE Phase 55 (2026-10-09):** the kernel image is now W^X (read-only .text/.rodata, NULL page unmapped, stack guard band) and in-kernel faults in apps/commands/desktop frames are CONTAINED by kguard (see [[phase55-kernel-usb-net-persist]]) — still one ring-0 address space, so this note about shared state/locks remains true.

**UPDATE 2026-10-08 (Phase 54): user-process ISOLATION now exists.** `uproc.c` gives each `runelf`'d binary its own page directory (ring 3, PAGE_USER user pages, CR3 switch, int-0x80 syscalls); a ring-3 fault kills just that process, OS survives. The KERNEL/desktop still run as one shared-address-space cooperative thread (that part of this note stands). See [[phase54-isolation-usb-folders]].

NexusOS has **no real process isolation** and this is foundational for any
security/sandboxing/multi-user work:

- All processes + the kernel share ONE 16 MB identity-mapped address space, a
  single global `page_directory` (paging.c), loaded into CR3 once at boot and
  never swapped. `context_switch` (switch.asm) only swaps stacks — same address
  space forever.
- Everything runs in **ring 0**. The GDT defines ring-3 selectors and there's a
  TSS, but no process uses them. `process_t.is_user`/`user_stack` (Phase 20) are
  dead fields. `vmm_fork()` builds a child page dir nobody stores or loads.
- So a process can freely read/write kernel memory and other processes' memory.

**What Phase 44 added (cooperative security, the honest slice):**
`process_t.uid`, `process_terminate_as(pid, caller_uid)` (only owner/root may
kill; pid 1 protected), VFS rwx perms keyed on the logged-in uid, salted-hash
login. These gate *who may act on* resources — NOT memory boundaries.

**Real isolation = a 2–3 day rewrite:** per-process page directories stored in
`process_t`, CR3 load in context_switch, ring-3 entry via IRET, per-process TSS
ESP0, and a VMM refactor to per-process address spaces. Don't claim memory
isolation / true sandboxing until that lands. Also note the known
[[intermittent-boot-doublefault]] lives in this same fragile
heap/stack/fb_back layout, so paging changes are extra risky. Build/test per
[[nexusos-boot-and-test]].
