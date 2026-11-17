---
name: nexusos-1080p-memory-layout
description: "Phase 53: native 1920x1080x32 support — memory map, backbuffer relocation, LFB-map-size #DF, VBE mode enumeration"
metadata: 
  node_type: memory
  type: project
  originSessionId: 06e1faaa-ba8d-4fed-9f7d-99de8d0b55b9
---

Phase 53 (2026-06-29) took NexusOS from a hardcoded 1024x768 to native 1920x1080x32.
Verified booting headless at 1080p, desktop fills the screen, stable. See
[[desktop-dirty-rect-render]] and [[headless-test-harness-gotchas]].

UPDATE 2026-10-08: the Aurora wallpaper cache occupies **0x01800000-0x02000000 (8MB)**,
reserved in pmm_init (memory.h WALLPAPER_CACHE_ADDR/SIZE); pmm_init also reserves heap,
boot stack and .bss now, detects RAM via CMOS and never allocates above the 32MB map.

MEMORY MAP (all must stay under the identity-map ceiling):
- kernel @ 0x100000 (1MB); heap 0x200000-0xA00000 (2-10MB, HEAP_MAX); demand-paging
  window 0x400000-0xF00000 (4-15MB, paging.c page_fault_handler).
- BACK buffer relocated 0x500000 -> **0x01000000 (16MB)**. At 1920x1080x4 it is ~8MB
  (to 0x017E9000), above heap + demand window, inside the identity map. Reserved in
  the PMM via new pmm_reserve_range() (called from fb_init, which runs after pmm_init)
  so the allocator never hands out those pages. framebuffer.c FB_BACKBUF_ADDR.
- IDENTITY MAP raised 16MB -> **32MB**: paging.c IDENTITY_TABLES=8 (one 4MB page table
  each). Everything the CPU touches (kernel/heap/backbuffer) must live below 32MB.

THE BOOT #DF (cost real time — root cause): paging_map_vesa_fb() mapped only ONE 4MB
page table for the hardware LFB (front buffer @ 0xFE000000). A 1920x1080x32 LFB is ~8MB,
so the first full-screen fb_flip() (kernel.c, before gpu_init, takes the legacy VESA
copy path that writes the WHOLE LFB) wrote past the 4MB mapping -> page fault -> #DF at
boot (serial showed INT=0x0e then INT=0x08). Fix: paging_map_vesa_fb(phys, SIZE) now maps
ceil(size/4MB) tables (VESA_FB_TABLES=4 provisioned); kernel.c passes fb_get_lfb_size().
Symptom was an all-black screen (backbuffer xp showed all zeros) + alive HLT loop.

BOOTLOADER (boot2.asm setup_vesa): rewritten to ENUMERATE VBE modes (4F00 -> mode-list
far ptr -> 4F01 per mode) and pick 1920x1080x32 with a linear-FB attribute, falling back
to mode 0x118 (1024x768x32) then a blind 0x4118 set. There is NO standard VBE mode number
for 1080p — must enumerate. QEMU virtio-vga DOES offer a 1080p linear mode; it also came
back 32bpp (vs the old 0x118 which QEMU gave as 24bpp), so the cheap 32bpp memcpy present
path is now active. Stored at 0x9000: addr/pitch/width/height/flag/bpp.

The GPU path (gpu.c) and all UI layout were already resolution-dynamic (fb_get_width/height),
so taskbar/widgets/icons auto-anchored correctly at the new size — no per-widget fixes needed.

GOTCHA — mouse cell-coordinate hit-tests break at non-768 heights. The mouse exposes both
pixel coords (ms.px/ms.py) and character-cell coords (ms.x=px/8, ms.y=py/16, mouse.c). Hit-tests
that compare ms.y against a FIXED cell row break when the resolution changes, because py/16 spans
0..67 at 1080p but layout constants like TASKBAR_ROW=GUI_HEIGHT-1=47 are fixed. This made ALL
taskbar clicks dead at 1080p (start menu, restore-minimized, tray) — symptom "taskbar not working,
can't minimize/restore/open menu". Window min/close/max buttons use PIXEL hit-tests (window_*_hit_px
via win_btn_left) and were fine; only the taskbar used a fixed cell row. Fix: taskbar_hit() tests
the actual pixel band (cell row >= (sh-TB_H)/16) in VESA mode. Desktop ICONS are safe because both
their draw and hit-test use the same cell grid from the top. Audit any other fixed-cell-row hit-test
before changing resolution again.
Remaining: the desktop now has lots of empty space / black void -> visual refresh is the
follow-up (modern flat Win11/macOS look the user asked for).
